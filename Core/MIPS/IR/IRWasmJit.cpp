// IR -> WebAssembly backend for the Emscripten build. See IRWasmJit.h.

#ifdef __EMSCRIPTEN__

#include <cstddef>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>

#include "Common/Log.h"
#include "Core/MemMap.h"
#include "Core/MIPS/MIPS.h"
#include "Core/MIPS/IR/IRInst.h"
#include "Core/MIPS/IR/IRJit.h"
#include "Core/MIPS/IR/IRWasmJit.h"

// Compiles `len` bytes at `ptr` into a module whose exports f0..f{count-1} go to the table slots in `slots`
// (0 = grow the table). Runs on the calling thread, which owns its own copy of the function table.
EM_JS(int, irwasm_install, (const uint8_t *ptr, int len, int *slots, int count), {
	try {
		var bytes = HEAPU8.slice(ptr, ptr + len);
		var inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), { e: { m: wasmMemory, t: wasmTable } });
		for (var i = 0; i < count; i++) {
			var idx = HEAP32[(slots >> 2) + i];
			if (idx == 0) {
				idx = wasmTable.length;
				wasmTable.grow(1);
				HEAP32[(slots >> 2) + i] = idx;
			}
			wasmTable.set(idx, inst.exports['f' + i]);
		}
		return 1;
	} catch (e) {
		err('IRWasmJit: ' + e);
		return 0;
	}
});

// Grows the calling thread's function table by `count` empty slots and returns the first index.
EM_JS(int, irwasm_reserve, (int count), {
	try {
		var base = wasmTable.length;
		wasmTable.grow(count);
		return base;
	} catch (e) {
		err('IRWasmJit: ' + e);
		return 0;
	}
});

// Identifies the JS realm (worker) of the calling thread: table slots are only valid in the realm that filled them.
EM_JS(int, irwasm_token, (), {
	if (!globalThis.__irwasmToken) globalThis.__irwasmToken = ((Math.random() * 2147483646) | 0) + 1;
	return globalThis.__irwasmToken;
});

namespace MIPSComp {

int IRWasmJit::mode = 1;
int IRWasmJit::stats[8];
// stats: 0 compiled blocks, 1 blocks with a fallback, 2 direct links (and install failures), 3 verify runs, 4 verify mismatches,
//        5 IR ops compiled natively, 6 IR ops left to the interpreter, 7 table slots in use

namespace {

enum : uint8_t {
	OP_UNREACHABLE = 0x00, OP_BLOCK = 0x02, OP_IF = 0x04, OP_ELSE = 0x05, OP_END = 0x0B, OP_RETURN = 0x0F,
	OP_CALL_INDIRECT = 0x11, OP_RETURN_CALL_INDIRECT = 0x13, OP_DROP = 0x1A, OP_SELECT = 0x1B,
	OP_LOCAL_GET = 0x20, OP_LOCAL_SET = 0x21, OP_LOCAL_TEE = 0x22,
	OP_I32_LOAD = 0x28, OP_I64_LOAD = 0x29, OP_F32_LOAD = 0x2A,
	OP_I32_LOAD8_S = 0x2C, OP_I32_LOAD8_U = 0x2D, OP_I32_LOAD16_S = 0x2E, OP_I32_LOAD16_U = 0x2F,
	OP_I32_STORE = 0x36, OP_I64_STORE = 0x37, OP_F32_STORE = 0x38, OP_I32_STORE8 = 0x3A, OP_I32_STORE16 = 0x3B,
	OP_I32_CONST = 0x41, OP_F32_CONST = 0x43,
	OP_I32_EQZ = 0x45, OP_I32_EQ = 0x46, OP_I32_NE = 0x47, OP_I32_LT_S = 0x48, OP_I32_LT_U = 0x49,
	OP_I32_GT_S = 0x4A, OP_I32_GT_U = 0x4B, OP_I32_LE_S = 0x4C, OP_I32_LE_U = 0x4D, OP_I32_GE_S = 0x4E,
	OP_F32_EQ = 0x5B, OP_F32_NE = 0x5C, OP_F32_LT = 0x5D, OP_F32_GT = 0x5E, OP_F32_LE = 0x5F, OP_F32_GE = 0x60,
	OP_I32_CLZ = 0x67, OP_I32_ADD = 0x6A, OP_I32_SUB = 0x6B, OP_I32_MUL_ = 0x6C, OP_I32_DIV_S = 0x6D, OP_I32_DIV_U = 0x6E,
	OP_I32_REM_S = 0x6F, OP_I32_REM_U = 0x70, OP_I32_AND = 0x71, OP_I32_OR = 0x72, OP_I32_XOR = 0x73,
	OP_I32_SHL = 0x74, OP_I32_SHR_S = 0x75, OP_I32_SHR_U = 0x76, OP_I32_ROTR = 0x78,
	OP_I64_CONST = 0x42, OP_I64_ADD = 0x7C, OP_I64_SUB = 0x7D, OP_I64_MUL = 0x7E, OP_I64_OR = 0x84, OP_I64_SHL = 0x86,
	OP_I64_SHR_U = 0x88, OP_I32_WRAP_I64 = 0xA7,
	OP_F32_ABS = 0x8B, OP_F32_NEG = 0x8C, OP_F32_SQRT = 0x91, OP_F32_ADD = 0x92, OP_F32_SUB = 0x93,
	OP_F32_MUL = 0x94, OP_F32_DIV = 0x95,
	OP_I64_EXTEND_I32_S = 0xAC, OP_I64_EXTEND_I32_U = 0xAD, OP_F32_CONVERT_I32_S = 0xB2,
	OP_I32_REINTERPRET_F32 = 0xBC, OP_F32_REINTERPRET_I32 = 0xBE,
	OP_I32_EXTEND8_S = 0xC0, OP_I32_EXTEND16_S = 0xC1,
	OP_SIMD = 0xFD,
	BT_VOID = 0x40, T_I32 = 0x7F, T_I64 = 0x7E, T_F32 = 0x7D,
};

// Locals of every generated function: params, then scratch.
enum : uint32_t {
	L_MIPS = 0, L_ARENA = 1,
	L_I0 = 2, L_I1, L_I2, L_I3, L_I4,
	L_L0 = 7,
	L_F0 = 8, L_F1, L_F2, L_F3,
	L_C0 = 12,  // first local of the register cache
};

const uint32_t MEM_MASK = 0x3FFFFFFF;

// Verify-mode store journal: count at [0], entries of 32 bytes from +16 (masked address, then 16 old bytes at +16).
const uint32_t JOURNAL_ENTRY = 32;
const uint32_t JOURNAL_MAX = 1024;
alignas(16) uint8_t g_journal[16 + JOURNAL_ENTRY * JOURNAL_MAX];
alignas(16) uint8_t g_journalNew[16 * JOURNAL_MAX];

struct Emitter {
	std::vector<uint8_t> b;

	void u8(uint8_t v) { b.push_back(v); }
	void uleb(uint32_t v) {
		do {
			uint8_t byte = v & 0x7F;
			v >>= 7;
			if (v) byte |= 0x80;
			b.push_back(byte);
		} while (v);
	}
	void sleb(int32_t v) {
		bool more = true;
		while (more) {
			uint8_t byte = v & 0x7F;
			v >>= 7;
			if ((v == 0 && !(byte & 0x40)) || (v == -1 && (byte & 0x40)))
				more = false;
			else
				byte |= 0x80;
			b.push_back(byte);
		}
	}
	void op(uint8_t o) { b.push_back(o); }
	void i32c(uint32_t v) { op(OP_I32_CONST); sleb((int32_t)v); }
	void get(uint32_t l) { op(OP_LOCAL_GET); uleb(l); }
	void set(uint32_t l) { op(OP_LOCAL_SET); uleb(l); }
	void tee(uint32_t l) { op(OP_LOCAL_TEE); uleb(l); }
	void mem(uint8_t o, uint32_t align, uint32_t offset) { op(o); uleb(align); uleb(offset); }
	void simdMem(uint32_t subop, uint32_t offset) { op(OP_SIMD); uleb(subop); uleb(0); uleb(offset); }
	void str(const char *s) { size_t n = strlen(s); uleb((uint32_t)n); b.insert(b.end(), s, s + n); }
	void bytes(const std::vector<uint8_t> &v) { b.insert(b.end(), v.begin(), v.end()); }
};

// MIPSState as an array of 32-bit words ("slots"). IR register numbers are slot numbers (r[] starts the state,
// see IRInst.h), f[i] is slot 32 + i, and the named fields (lo, hi, pc...) are slots too.
inline uint32_t SR(uint32_t i) { return i; }
inline uint32_t SF(uint32_t i) { return (uint32_t)offsetof(MIPSState, f) / 4 + i; }
const uint32_t S_LO = (uint32_t)offsetof(MIPSState, lo) / 4;
const uint32_t S_HI = (uint32_t)offsetof(MIPSState, hi) / 4;
const uint32_t S_PC = (uint32_t)offsetof(MIPSState, pc) / 4;
const uint32_t S_FCR31 = (uint32_t)offsetof(MIPSState, fcr31) / 4;
const uint32_t S_FPCOND = (uint32_t)offsetof(MIPSState, fpcond) / 4;
const uint32_t S_DOWNCOUNT = (uint32_t)offsetof(MIPSState, downcount) / 4;
const uint32_t S_LLBIT = (uint32_t)offsetof(MIPSState, llBit) / 4;
inline uint32_t SV(uint32_t i) { return (uint32_t)offsetof(MIPSState, vfpuCtrl) / 4 + i; }
const uint32_t NUM_SLOTS = (uint32_t)offsetof(MIPSState, debugCount) / 4;

static_assert(offsetof(MIPSState, r) == 0, "IR register numbers assume r[] at the start of MIPSState");
static_assert(offsetof(MIPSState, lo) == offsetof(MIPSState, pc) + 4 && offsetof(MIPSState, hi) == offsetof(MIPSState, lo) + 4, "lo/hi");
static_assert(offsetof(MIPSState, downcount) % 4 == 0 && offsetof(MIPSState, llBit) % 4 == 0, "slot alignment");
static_assert(sizeof(IRInst) == 8, "IRInst layout");

class BlockCompiler {
public:
	// Table slot of the compiled block at a PSP address (and the emuhack word it was compiled for), or 0.
	typedef std::function<int(u32 target, u32 *word)> LinkResolver;

	// journal: address of the store journal (verify mode), or 0. link: resolver for direct exits, or empty.
	BlockCompiler(Emitter &e, uint32_t memBase, uint32_t interpIdx, uint32_t journal, LinkResolver link)
		: out_(e), base_(memBase), interp_(interpIdx), journal_(journal), link_(link) {
		memset(local_, 0, sizeof(local_));
		memset(dirty_, 0, sizeof(dirty_));
	}

	// Emits one function body (locals + code). Returns false when the first op is not native.
	bool Compile(const IRInst *arena, uint32_t start, uint32_t end, bool *pure) {
		bool isPure = true;
		uint32_t native = 0;
		uint32_t stores = 0;
		uint32_t i = start;
		for (; i < end; i++) {
			const IRInst &inst = arena[i];
			if (IsStore(inst.op) && (!journal_ || ++stores > JOURNAL_MAX))
				isPure = false;
			if (!Op(inst))
				break;
			native++;
		}
		if (i < end || !endsWithExit_) {
			// Hand the rest of the block (or the fall-through) to the interpreter.
			Fallback(i);
			isPure = false;
			IRWasmJit::stats[1]++;
		}
		IRWasmJit::stats[5] += native;
		IRWasmJit::stats[6] += end - i;
		e_.op(OP_END);
		if (bad_)
			return false;

		// Locals: 5 x i32, 1 x i64, 4 x f32, then one i32 per cached slot.
		Emitter body;
		body.uleb(numCached_ ? 4 : 3);
		body.uleb(5); body.u8(T_I32);
		body.uleb(1); body.u8(T_I64);
		body.uleb(4); body.u8(T_F32);
		if (numCached_) { body.uleb(numCached_); body.u8(T_I32); }
		body.bytes(e_.b);
		out_.uleb((uint32_t)body.b.size());
		out_.bytes(body.b);
		*pure = isPure;
		return native > 0;
	}

private:
	Emitter &out_;
	Emitter e_;
	uint32_t base_;
	uint32_t interp_;
	uint32_t journal_;
	LinkResolver link_;
	bool endsWithExit_ = false;
	bool bad_ = false;  // an op used a register outside MIPSState: leave the block to the interpreter
	int branchDepth_ = 0;

	// Slot cache: local_[slot] = local index (0 = not cached), dirty_ = local newer than MIPSState.
	uint32_t local_[NUM_SLOTS];
	bool dirty_[NUM_SLOTS];
	std::vector<uint32_t> cached_;
	uint32_t numCached_ = 0;

	static bool IsStore(IROp op) {
		switch (op) {
		case IROp::Store8: case IROp::Store16: case IROp::Store32: case IROp::Store32Left: case IROp::Store32Right:
		case IROp::Store32Conditional: case IROp::StoreFloat: case IROp::StoreVec4:
			return true;
		default:
			return false;
		}
	}

	// ---- Slot cache ----

	uint32_t NewLocal(uint32_t slot) {
		local_[slot] = L_C0 + numCached_++;
		cached_.push_back(slot);
		return local_[slot];
	}
	// Push the slot as i32.
	void get(uint32_t slot) {
		if (slot >= NUM_SLOTS) { bad_ = true; e_.i32c(0); return; }
		if (local_[slot]) {
			e_.get(local_[slot]);
		} else if (branchDepth_ > 0) {
			// Not cached, so MIPSState is current. Don't start caching in only one path of a branch.
			e_.get(L_MIPS); e_.mem(OP_I32_LOAD, 2, slot * 4);
		} else {
			e_.get(L_MIPS); e_.mem(OP_I32_LOAD, 2, slot * 4); e_.tee(NewLocal(slot));
		}
	}
	void getF(uint32_t slot) { get(slot); e_.op(OP_F32_REINTERPRET_I32); }
	// Pop an i32 into the slot.
	void put(uint32_t slot) {
		if (slot >= NUM_SLOTS) { bad_ = true; e_.op(OP_DROP); return; }
		if (!local_[slot]) {
			_dbg_assert_(branchDepth_ == 0);
			NewLocal(slot);
		}
		e_.set(local_[slot]);
		dirty_[slot] = true;
	}
	void putF(uint32_t slot) { e_.op(OP_I32_REINTERPRET_F32); put(slot); }
	// Before a branch that writes (or reads) these slots: make them cached in both paths.
	void Prepare(std::initializer_list<uint32_t> slots) {
		for (uint32_t slot : slots) {
			if (slot < NUM_SLOTS && !local_[slot]) {
				e_.get(L_MIPS); e_.mem(OP_I32_LOAD, 2, slot * 4); e_.set(NewLocal(slot));
			}
		}
	}
	void If(uint8_t type = BT_VOID) { e_.op(OP_IF); e_.u8(type); branchDepth_++; }
	void Else() { e_.op(OP_ELSE); }
	void End() { e_.op(OP_END); branchDepth_--; }
	// Write dirty slots back (they stay cached and dirty for the code that follows in this path).
	void Flush() {
		for (uint32_t slot : cached_) {
			if (dirty_[slot]) {
				e_.get(L_MIPS); e_.get(local_[slot]); e_.mem(OP_I32_STORE, 2, slot * 4);
			}
		}
	}

	void Fallback(uint32_t index) {
		Flush();
		e_.get(L_MIPS);
		e_.get(L_ARENA);
		e_.i32c(index * (uint32_t)sizeof(IRInst));
		e_.op(OP_I32_ADD);
		e_.i32c(interp_);
		e_.op(OP_CALL_INDIRECT); e_.uleb(0); e_.uleb(0);
		e_.op(OP_RETURN);
	}

	// ---- PSP memory ----

	// Verify mode: before a store to the masked PSP address in local `l`, append {address, 16 old bytes} to the
	// journal so the block's writes can be undone and replayed through the interpreter.
	void Journal(uint32_t l) {
		if (!journal_)
			return;
		e_.i32c(journal_); e_.mem(OP_I32_LOAD, 2, 0); e_.i32c(JOURNAL_ENTRY); e_.op(OP_I32_MUL_);
		e_.i32c(journal_ + 16); e_.op(OP_I32_ADD); e_.set(L_I4);
		e_.get(L_I4); e_.get(l); e_.mem(OP_I32_STORE, 2, 0);
		e_.get(L_I4); e_.get(l); e_.simdMem(0x00, base_); e_.simdMem(0x0B, 16);
		e_.i32c(journal_); e_.i32c(journal_); e_.mem(OP_I32_LOAD, 2, 0); e_.i32c(1); e_.op(OP_I32_ADD); e_.mem(OP_I32_STORE, 2, 0);
	}
	// PSP address -> (x & MEM_MASK) on the stack; Memory::base goes into the memarg offset.
	void addr(const IRInst &inst, uint32_t mask = MEM_MASK) {
		get(SR(inst.src1));
		if (inst.constant) { e_.i32c(inst.constant); e_.op(OP_I32_ADD); }
		e_.i32c(mask); e_.op(OP_I32_AND);
	}
	void storeAddr(const IRInst &inst) { addr(inst); e_.set(L_I3); Journal(L_I3); e_.get(L_I3); }

	// ---- Op helpers ----

	void binR(const IRInst &inst, uint8_t o) { get(SR(inst.src1)); get(SR(inst.src2)); e_.op(o); put(SR(inst.dest)); }
	void binRC(const IRInst &inst, uint8_t o) { get(SR(inst.src1)); e_.i32c(inst.constant); e_.op(o); put(SR(inst.dest)); }
	void unR(const IRInst &inst, uint8_t o) { get(SR(inst.src1)); e_.op(o); put(SR(inst.dest)); }
	void shiftImm(const IRInst &inst, uint8_t o) { get(SR(inst.src1)); e_.i32c(inst.src2); e_.op(o); put(SR(inst.dest)); }
	void shiftReg(const IRInst &inst, uint8_t o) {
		get(SR(inst.src1)); get(SR(inst.src2)); e_.i32c(31); e_.op(OP_I32_AND); e_.op(o); put(SR(inst.dest));
	}
	void binF(const IRInst &inst, uint8_t o) { getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(o); putF(SF(inst.dest)); }
	void unF(const IRInst &inst, uint8_t o) { getF(SF(inst.src1)); e_.op(o); putF(SF(inst.dest)); }
	// Leave the block for a fixed PSP address. When that block is compiled too, jump straight into it (tail call)
	// instead of returning to the dispatcher, under the same conditions the dispatcher loop has: downcount >= 0
	// and the emuhack at the target still being the one the link was made for (the block wasn't invalidated).
	void exitTo(u32 target) {
		Flush();
		u32 word = 0;
		int slot = link_ && Memory::IsValid4AlignedAddress(target) ? link_(target, &word) : 0;
		if (slot > 0) {
			get(S_DOWNCOUNT); e_.i32c(0); e_.op(OP_I32_GE_S);
			e_.i32c(0); e_.mem(OP_I32_LOAD, 0, base_ + (target & MEM_MASK)); e_.i32c(word); e_.op(OP_I32_EQ);
			e_.op(OP_I32_AND);
			If();
			e_.get(L_MIPS); e_.i32c(target); e_.mem(OP_I32_STORE, 2, S_PC * 4);
			e_.get(L_MIPS); e_.get(L_ARENA); e_.i32c((uint32_t)slot);
			e_.op(OP_RETURN_CALL_INDIRECT); e_.uleb(0); e_.uleb(0);
			End();
			IRWasmJit::stats[2]++;
		}
		e_.i32c(target); e_.op(OP_RETURN);
	}
	void exitIf(const IRInst &inst, uint8_t cmp, bool withZero) {
		get(SR(inst.src1));
		if (withZero) e_.i32c(0); else get(SR(inst.src2));
		e_.op(cmp);
		If();
		exitTo(inst.constant);
		End();
	}
	// Vec4 lane by lane, in the same order as the C loops of the interpreter (dest may alias a source).
	void vec4F(const IRInst &inst, uint8_t o) {
		for (uint32_t i = 0; i < 4; i++) { getF(SF(inst.src1 + i)); getF(SF(inst.src2 + i)); e_.op(o); putF(SF(inst.dest + i)); }
	}
	void vec4UnF(const IRInst &inst, uint8_t o) {
		for (uint32_t i = 0; i < 4; i++) { getF(SF(inst.src1 + i)); e_.op(o); putF(SF(inst.dest + i)); }
	}
	// Read n slots first, then write them (for ops where dest may overlap the sources).
	void copyLanes(const uint32_t *src, uint32_t dest, uint32_t n) {
		for (uint32_t i = 0; i < n; i++) { get(src[i]); e_.set(L_I0 + i); }
		for (uint32_t i = 0; i < n; i++) { e_.get(L_I0 + i); put(dest + i); }
	}
	// my_isnan / my_isinf / my_isnanorinf on the bits in local l.
	void isNanBits(uint32_t l) { e_.get(l); e_.i32c(0x7FFFFFFF); e_.op(OP_I32_AND); e_.i32c(0x7F800000); e_.op(OP_I32_GT_U); }
	void isInfBits(uint32_t l) { e_.get(l); e_.i32c(0x7FFFFFFF); e_.op(OP_I32_AND); e_.i32c(0x7F800000); e_.op(OP_I32_EQ); }
	void isNanOrInfBits(uint32_t l) { e_.get(l); e_.i32c(0x7F800000); e_.op(OP_I32_AND); e_.i32c(0x7F800000); e_.op(OP_I32_EQ); }
	void product64(const IRInst &inst, bool isSigned) {
		get(SR(inst.src1)); e_.op(isSigned ? OP_I64_EXTEND_I32_S : OP_I64_EXTEND_I32_U);
		get(SR(inst.src2)); e_.op(isSigned ? OP_I64_EXTEND_I32_S : OP_I64_EXTEND_I32_U);
		e_.op(OP_I64_MUL);
	}
	// lo/hi <- the i64 in L_L0 (memcpy(&mips->lo, &result, 8) in the interpreter).
	void putLoHi() {
		e_.get(L_L0); e_.op(OP_I32_WRAP_I64); put(S_LO);
		e_.get(L_L0); e_.op(OP_I64_CONST); e_.sleb(32); e_.op(OP_I64_SHR_U); e_.op(OP_I32_WRAP_I64); put(S_HI);
	}
	void F32Const(float f) {
		uint32_t u;
		memcpy(&u, &f, 4);
		e_.op(OP_F32_CONST);
		for (int i = 0; i < 4; i++) e_.u8((u >> (8 * i)) & 0xFF);
	}

	bool Op(const IRInst &inst) {
		endsWithExit_ = false;
		switch (inst.op) {
		case IROp::SetConst: e_.i32c(inst.constant); put(SR(inst.dest)); return true;
		case IROp::SetConstF: e_.i32c(inst.constant); put(SF(inst.dest)); return true;
		case IROp::Add: binR(inst, OP_I32_ADD); return true;
		case IROp::Sub: binR(inst, OP_I32_SUB); return true;
		case IROp::And: binR(inst, OP_I32_AND); return true;
		case IROp::Or: binR(inst, OP_I32_OR); return true;
		case IROp::Xor: binR(inst, OP_I32_XOR); return true;
		case IROp::Mov: get(SR(inst.src1)); put(SR(inst.dest)); return true;
		case IROp::AddConst: binRC(inst, OP_I32_ADD); return true;
		case IROp::SubConst: binRC(inst, OP_I32_SUB); return true;
		case IROp::AndConst: binRC(inst, OP_I32_AND); return true;
		case IROp::OrConst: binRC(inst, OP_I32_OR); return true;
		case IROp::XorConst: binRC(inst, OP_I32_XOR); return true;
		case IROp::OptAddConst: get(SR(inst.dest)); e_.i32c(inst.constant); e_.op(OP_I32_ADD); put(SR(inst.dest)); return true;
		case IROp::OptAndConst: get(SR(inst.dest)); e_.i32c(inst.constant); e_.op(OP_I32_AND); put(SR(inst.dest)); return true;
		case IROp::OptOrConst: get(SR(inst.dest)); e_.i32c(inst.constant); e_.op(OP_I32_OR); put(SR(inst.dest)); return true;
		case IROp::Neg: e_.i32c(0); get(SR(inst.src1)); e_.op(OP_I32_SUB); put(SR(inst.dest)); return true;
		case IROp::Not: get(SR(inst.src1)); e_.i32c(0xFFFFFFFF); e_.op(OP_I32_XOR); put(SR(inst.dest)); return true;
		case IROp::Ext8to32: unR(inst, OP_I32_EXTEND8_S); return true;
		case IROp::Ext16to32: unR(inst, OP_I32_EXTEND16_S); return true;

		case IROp::Load8: addr(inst); e_.mem(OP_I32_LOAD8_U, 0, base_); put(SR(inst.dest)); return true;
		case IROp::Load8Ext: addr(inst); e_.mem(OP_I32_LOAD8_S, 0, base_); put(SR(inst.dest)); return true;
		case IROp::Load16: addr(inst); e_.mem(OP_I32_LOAD16_U, 0, base_); put(SR(inst.dest)); return true;
		case IROp::Load16Ext: addr(inst); e_.mem(OP_I32_LOAD16_S, 0, base_); put(SR(inst.dest)); return true;
		case IROp::Load32: addr(inst); e_.mem(OP_I32_LOAD, 0, base_); put(SR(inst.dest)); return true;
		case IROp::LoadFloat: addr(inst); e_.mem(OP_I32_LOAD, 0, base_); put(SF(inst.dest)); return true;
		case IROp::Load32Linked:
			if (inst.dest != MIPS_REG_ZERO) { addr(inst); e_.mem(OP_I32_LOAD, 0, base_); put(SR(inst.dest)); }
			e_.i32c(1); put(S_LLBIT);
			return true;
		case IROp::Load32Left:
		case IROp::Load32Right:
			// I0 = addr, I1 = shift, I2 = mem
			get(SR(inst.src1)); if (inst.constant) { e_.i32c(inst.constant); e_.op(OP_I32_ADD); } e_.set(L_I0);
			e_.get(L_I0); e_.i32c(3); e_.op(OP_I32_AND); e_.i32c(3); e_.op(OP_I32_SHL); e_.set(L_I1);
			e_.get(L_I0); e_.i32c(MEM_MASK & 0xFFFFFFFC); e_.op(OP_I32_AND); e_.mem(OP_I32_LOAD, 0, base_); e_.set(L_I2);
			get(SR(inst.dest));
			if (inst.op == IROp::Load32Left) {
				// (dest & (0x00ffffff >> shift)) | (mem << (24 - shift))
				e_.i32c(0x00FFFFFF); e_.get(L_I1); e_.op(OP_I32_SHR_U); e_.op(OP_I32_AND);
				e_.get(L_I2); e_.i32c(24); e_.get(L_I1); e_.op(OP_I32_SUB); e_.op(OP_I32_SHL);
			} else {
				// (dest & (0xffffff00 << (24 - shift))) | (mem >> shift)
				e_.i32c(0xFFFFFF00); e_.i32c(24); e_.get(L_I1); e_.op(OP_I32_SUB); e_.op(OP_I32_SHL); e_.op(OP_I32_AND);
				e_.get(L_I2); e_.get(L_I1); e_.op(OP_I32_SHR_U);
			}
			e_.op(OP_I32_OR);
			put(SR(inst.dest));
			return true;

		case IROp::Store8: storeAddr(inst); get(SR(inst.src3)); e_.mem(OP_I32_STORE8, 0, base_); return true;
		case IROp::Store16: storeAddr(inst); get(SR(inst.src3)); e_.mem(OP_I32_STORE16, 0, base_); return true;
		case IROp::Store32: storeAddr(inst); get(SR(inst.src3)); e_.mem(OP_I32_STORE, 0, base_); return true;
		case IROp::StoreFloat: storeAddr(inst); get(SF(inst.src3)); e_.mem(OP_I32_STORE, 0, base_); return true;
		case IROp::Store32Left:
		case IROp::Store32Right:
			// I0 = addr, I1 = shift, I3 = aligned masked address
			get(SR(inst.src1)); if (inst.constant) { e_.i32c(inst.constant); e_.op(OP_I32_ADD); } e_.set(L_I0);
			e_.get(L_I0); e_.i32c(3); e_.op(OP_I32_AND); e_.i32c(3); e_.op(OP_I32_SHL); e_.set(L_I1);
			e_.get(L_I0); e_.i32c(MEM_MASK & 0xFFFFFFFC); e_.op(OP_I32_AND); e_.set(L_I3);
			Journal(L_I3);
			e_.get(L_I3);
			if (inst.op == IROp::Store32Left) {
				// (src3 >> (24 - shift)) | (mem & (0xffffff00 << shift))
				get(SR(inst.src3)); e_.i32c(24); e_.get(L_I1); e_.op(OP_I32_SUB); e_.op(OP_I32_SHR_U);
				e_.get(L_I3); e_.mem(OP_I32_LOAD, 0, base_); e_.i32c(0xFFFFFF00); e_.get(L_I1); e_.op(OP_I32_SHL); e_.op(OP_I32_AND);
			} else {
				// (src3 << shift) | (mem & (0x00ffffff >> (24 - shift)))
				get(SR(inst.src3)); e_.get(L_I1); e_.op(OP_I32_SHL);
				e_.get(L_I3); e_.mem(OP_I32_LOAD, 0, base_); e_.i32c(0x00FFFFFF); e_.i32c(24); e_.get(L_I1); e_.op(OP_I32_SUB); e_.op(OP_I32_SHR_U); e_.op(OP_I32_AND);
			}
			e_.op(OP_I32_OR);
			e_.mem(OP_I32_STORE, 0, base_);
			return true;
		case IROp::Store32Conditional:
			if (inst.dest != MIPS_REG_ZERO) Prepare({ SR(inst.dest) });
			get(S_LLBIT);
			If();
			storeAddr(inst); get(SR(inst.src3)); e_.mem(OP_I32_STORE, 0, base_);
			if (inst.dest != MIPS_REG_ZERO) {
				e_.i32c(1); put(SR(inst.dest));
				Else();
				e_.i32c(0); put(SR(inst.dest));
			}
			End();
			return true;

		case IROp::LoadVec4:
			// memcpy(&f[dest], mem, 16): four words, all read before any write.
			addr(inst); e_.set(L_I3);
			for (uint32_t i = 0; i < 4; i++) { e_.get(L_I3); e_.mem(OP_I32_LOAD, 0, base_ + 4 * i); put(SF(inst.dest + i)); }
			return true;
		case IROp::StoreVec4:
			storeAddr(inst); e_.set(L_I3);
			for (uint32_t i = 0; i < 4; i++) { e_.get(L_I3); get(SF(inst.dest + i)); e_.mem(OP_I32_STORE, 0, base_ + 4 * i); }
			return true;

		case IROp::Vec4Init:
		{
			static const uint32_t init[7][4] = {
				{ 0, 0, 0, 0 },
				{ 0x3F800000, 0x3F800000, 0x3F800000, 0x3F800000 },
				{ 0xBF800000, 0xBF800000, 0xBF800000, 0xBF800000 },
				{ 0x3F800000, 0, 0, 0 },
				{ 0, 0x3F800000, 0, 0 },
				{ 0, 0, 0x3F800000, 0 },
				{ 0, 0, 0, 0x3F800000 },
			};
			if (inst.src1 >= 7)
				return false;
			for (uint32_t i = 0; i < 4; i++) { e_.i32c(init[inst.src1][i]); put(SF(inst.dest + i)); }
			return true;
		}
		case IROp::Vec4Shuffle:
		{
			uint32_t src[4];
			for (uint32_t i = 0; i < 4; i++) src[i] = SF(inst.src1 + ((inst.src2 >> (i * 2)) & 3));
			copyLanes(src, SF(inst.dest), 4);
			return true;
		}
		case IROp::Vec4Blend:
		{
			uint32_t src[4];
			for (uint32_t i = 0; i < 4; i++) src[i] = ((inst.constant >> i) & 1) ? SF(inst.src2 + i) : SF(inst.src1 + i);
			copyLanes(src, SF(inst.dest), 4);
			return true;
		}
		case IROp::Vec4Mov:
		{
			uint32_t src[4] = { SF(inst.src1), SF(inst.src1 + 1), SF(inst.src1 + 2), SF(inst.src1 + 3) };
			copyLanes(src, SF(inst.dest), 4);
			return true;
		}
		case IROp::Vec4Add: vec4F(inst, OP_F32_ADD); return true;
		case IROp::Vec4Sub: vec4F(inst, OP_F32_SUB); return true;
		case IROp::Vec4Mul: vec4F(inst, OP_F32_MUL); return true;
		case IROp::Vec4Div: vec4F(inst, OP_F32_DIV); return true;
		case IROp::Vec4Scale:
			getF(SF(inst.src2)); e_.set(L_F0);
			for (uint32_t i = 0; i < 4; i++) { getF(SF(inst.src1 + i)); e_.get(L_F0); e_.op(OP_F32_MUL); putF(SF(inst.dest + i)); }
			return true;
		case IROp::Vec4Neg: vec4UnF(inst, OP_F32_NEG); return true;
		case IROp::Vec4Abs: vec4UnF(inst, OP_F32_ABS); return true;
		case IROp::Vec4Dot:
			// a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3], left to right.
			for (uint32_t i = 0; i < 4; i++) {
				getF(SF(inst.src1 + i)); getF(SF(inst.src2 + i)); e_.op(OP_F32_MUL);
				if (i > 0) e_.op(OP_F32_ADD);
			}
			putF(SF(inst.dest));
			return true;
		case IROp::Vec2Unpack16To31:
		case IROp::Vec2Unpack16To32:
		{
			bool to31 = inst.op == IROp::Vec2Unpack16To31;
			get(SF(inst.src1)); e_.i32c(16); e_.op(OP_I32_SHL); if (to31) { e_.i32c(1); e_.op(OP_I32_SHR_U); } e_.set(L_I0);
			get(SF(inst.src1)); e_.i32c(0xFFFF0000); e_.op(OP_I32_AND); if (to31) { e_.i32c(1); e_.op(OP_I32_SHR_U); } e_.set(L_I1);
			e_.get(L_I0); put(SF(inst.dest));
			e_.get(L_I1); put(SF(inst.dest + 1));
			return true;
		}
		case IROp::Vec4Unpack8To32:
			get(SF(inst.src1)); e_.set(L_I0);
			e_.get(L_I0); e_.i32c(24); e_.op(OP_I32_SHL); put(SF(inst.dest));
			e_.get(L_I0); e_.i32c(16); e_.op(OP_I32_SHL); e_.i32c(0xFF000000); e_.op(OP_I32_AND); put(SF(inst.dest + 1));
			e_.get(L_I0); e_.i32c(8); e_.op(OP_I32_SHL); e_.i32c(0xFF000000); e_.op(OP_I32_AND); put(SF(inst.dest + 2));
			e_.get(L_I0); e_.i32c(0xFF000000); e_.op(OP_I32_AND); put(SF(inst.dest + 3));
			return true;
		case IROp::Vec2Pack32To16:
			get(SF(inst.src1)); e_.i32c(16); e_.op(OP_I32_SHR_U);
			get(SF(inst.src1 + 1)); e_.i32c(0xFFFF0000); e_.op(OP_I32_AND);
			e_.op(OP_I32_OR); put(SF(inst.dest));
			return true;
		case IROp::Vec2Pack31To16:
			get(SF(inst.src1)); e_.i32c(15); e_.op(OP_I32_SHR_U); e_.i32c(0xFFFF); e_.op(OP_I32_AND);
			get(SF(inst.src1 + 1)); e_.i32c(1); e_.op(OP_I32_SHL); e_.i32c(0xFFFF0000); e_.op(OP_I32_AND);
			e_.op(OP_I32_OR); put(SF(inst.dest));
			return true;
		case IROp::Vec4Pack32To8:
			get(SF(inst.src1)); e_.i32c(24); e_.op(OP_I32_SHR_U);
			get(SF(inst.src1 + 1)); e_.i32c(16); e_.op(OP_I32_SHR_U); e_.i32c(0xFF00); e_.op(OP_I32_AND); e_.op(OP_I32_OR);
			get(SF(inst.src1 + 2)); e_.i32c(8); e_.op(OP_I32_SHR_U); e_.i32c(0xFF0000); e_.op(OP_I32_AND); e_.op(OP_I32_OR);
			get(SF(inst.src1 + 3)); e_.i32c(0xFF000000); e_.op(OP_I32_AND); e_.op(OP_I32_OR);
			put(SF(inst.dest));
			return true;
		case IROp::Vec4Pack31To8:
			get(SF(inst.src1)); e_.i32c(23); e_.op(OP_I32_SHR_U); e_.i32c(0xFF); e_.op(OP_I32_AND);
			get(SF(inst.src1 + 1)); e_.i32c(15); e_.op(OP_I32_SHR_U); e_.i32c(0xFF00); e_.op(OP_I32_AND); e_.op(OP_I32_OR);
			get(SF(inst.src1 + 2)); e_.i32c(7); e_.op(OP_I32_SHR_U); e_.i32c(0xFF0000); e_.op(OP_I32_AND); e_.op(OP_I32_OR);
			get(SF(inst.src1 + 3)); e_.i32c(1); e_.op(OP_I32_SHL); e_.i32c(0xFF000000); e_.op(OP_I32_AND); e_.op(OP_I32_OR);
			put(SF(inst.dest));
			return true;
		case IROp::Vec2ClampToZero:
		case IROp::Vec4ClampToZero:
		{
			uint32_t n = inst.op == IROp::Vec2ClampToZero ? 2 : 4;
			for (uint32_t i = 0; i < n; i++) { get(SF(inst.src1 + i)); e_.set(L_I0 + i); }
			for (uint32_t i = 0; i < n; i++) {
				// (int)v >= 0 ? v : 0
				e_.get(L_I0 + i); e_.i32c(0); e_.get(L_I0 + i); e_.i32c(0); e_.op(OP_I32_GE_S); e_.op(OP_SELECT);
				put(SF(inst.dest + i));
			}
			return true;
		}

		case IROp::FCmpVfpuBit:
		{
			const int vop = inst.dest & 0xF;
			const uint32_t bit = inst.dest >> 4;
			const uint32_t cc = SV(VFPU_CTRL_CC);
			Prepare({ cc });
			get(SF(inst.src1)); e_.set(L_I0);
			switch (vop) {
			case VC_EQ: getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_EQ); break;
			case VC_NE: getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_NE); break;
			case VC_LT: getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_LT); break;
			case VC_LE: getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_LE); break;
			case VC_GT: getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_GT); break;
			case VC_GE: getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_GE); break;
			case VC_EZ: getF(SF(inst.src1)); F32Const(0.0f); e_.op(OP_F32_EQ); break;
			case VC_NZ: getF(SF(inst.src1)); F32Const(0.0f); e_.op(OP_F32_NE); break;
			case VC_EN: isNanBits(L_I0); break;
			case VC_NN: isNanBits(L_I0); e_.op(OP_I32_EQZ); break;
			case VC_EI: isInfBits(L_I0); break;
			case VC_NI: isInfBits(L_I0); e_.op(OP_I32_EQZ); break;
			case VC_ES: isNanOrInfBits(L_I0); break;
			case VC_NS: isNanOrInfBits(L_I0); e_.op(OP_I32_EQZ); break;
			case VC_TR: e_.i32c(1); break;
			default: e_.i32c(0); break;
			}
			// cc = result ? cc | bit : cc & ~bit
			e_.set(L_I1);
			get(cc); e_.i32c(1u << bit); e_.op(OP_I32_OR);
			get(cc); e_.i32c(~(1u << bit)); e_.op(OP_I32_AND);
			e_.get(L_I1); e_.op(OP_SELECT);
			put(cc);
			return true;
		}
		case IROp::FCmpVfpuAggregate:
		{
			const uint32_t mask = inst.dest;
			const uint32_t cc = SV(VFPU_CTRL_CC);
			get(cc); e_.set(L_I0);
			e_.get(L_I0); e_.i32c(~0x30u); e_.op(OP_I32_AND);
			e_.i32c(0x10); e_.i32c(0); e_.get(L_I0); e_.i32c(mask); e_.op(OP_I32_AND); e_.op(OP_SELECT); e_.op(OP_I32_OR);
			e_.i32c(0x20); e_.i32c(0); e_.get(L_I0); e_.i32c(mask); e_.op(OP_I32_AND); e_.i32c(mask); e_.op(OP_I32_EQ); e_.op(OP_SELECT); e_.op(OP_I32_OR);
			put(cc);
			return true;
		}
		case IROp::FCmovVfpuCC:
			// dest = cond ? src1 : dest
			get(SF(inst.src1));
			get(SF(inst.dest));
			get(SV(VFPU_CTRL_CC)); e_.i32c(inst.src2 & 0xF); e_.op(OP_I32_SHR_U); e_.i32c(1); e_.op(OP_I32_AND);
			e_.i32c((uint32_t)inst.src2 >> 7); e_.op(OP_I32_EQ);
			e_.op(OP_SELECT);
			put(SF(inst.dest));
			return true;

		case IROp::FRSqrt: F32Const(1.0f); getF(SF(inst.src1)); e_.op(OP_F32_SQRT); e_.op(OP_F32_DIV); putF(SF(inst.dest)); return true;
		case IROp::FRecip: F32Const(1.0f); getF(SF(inst.src1)); e_.op(OP_F32_DIV); putF(SF(inst.dest)); return true;

		case IROp::ShlImm: shiftImm(inst, OP_I32_SHL); return true;
		case IROp::ShrImm: shiftImm(inst, OP_I32_SHR_U); return true;
		case IROp::SarImm: shiftImm(inst, OP_I32_SHR_S); return true;
		case IROp::RorImm: shiftImm(inst, OP_I32_ROTR); return true;
		case IROp::Shl: shiftReg(inst, OP_I32_SHL); return true;
		case IROp::Shr: shiftReg(inst, OP_I32_SHR_U); return true;
		case IROp::Sar: shiftReg(inst, OP_I32_SHR_S); return true;
		case IROp::Ror: shiftReg(inst, OP_I32_ROTR); return true;
		case IROp::Clz: unR(inst, OP_I32_CLZ); return true;

		case IROp::Slt: binR(inst, OP_I32_LT_S); return true;
		case IROp::SltU: binR(inst, OP_I32_LT_U); return true;
		case IROp::SltConst: binRC(inst, OP_I32_LT_S); return true;
		case IROp::SltUConst: binRC(inst, OP_I32_LT_U); return true;
		case IROp::MovZ:
		case IROp::MovNZ:
			// dest = (src1 == 0) ? src2 : dest  (MovZ), != 0 for MovNZ
			get(SR(inst.src2));
			get(SR(inst.dest));
			get(SR(inst.src1));
			if (inst.op == IROp::MovZ) e_.op(OP_I32_EQZ);
			e_.op(OP_SELECT);
			put(SR(inst.dest));
			return true;
		case IROp::Max:
		case IROp::Min:
			get(SR(inst.src1)); get(SR(inst.src2));
			get(SR(inst.src1)); get(SR(inst.src2)); e_.op(inst.op == IROp::Max ? OP_I32_GT_S : OP_I32_LT_S);
			e_.op(OP_SELECT);
			put(SR(inst.dest));
			return true;

		case IROp::MtLo: get(SR(inst.src1)); put(S_LO); return true;
		case IROp::MtHi: get(SR(inst.src1)); put(S_HI); return true;
		case IROp::MfLo: get(S_LO); put(SR(inst.dest)); return true;
		case IROp::MfHi: get(S_HI); put(SR(inst.dest)); return true;
		case IROp::Mult:
		case IROp::MultU:
			product64(inst, inst.op == IROp::Mult); e_.set(L_L0);
			putLoHi();
			return true;
		case IROp::Madd:
		case IROp::MaddU:
		case IROp::Msub:
		case IROp::MsubU:
		{
			bool isSigned = inst.op == IROp::Madd || inst.op == IROp::Msub;
			bool isSub = inst.op == IROp::Msub || inst.op == IROp::MsubU;
			// ((u64)hi << 32 | lo) +/- product
			get(S_LO); e_.op(OP_I64_EXTEND_I32_U);
			get(S_HI); e_.op(OP_I64_EXTEND_I32_U); e_.op(OP_I64_CONST); e_.sleb(32); e_.op(OP_I64_SHL);
			e_.op(OP_I64_OR);
			product64(inst, isSigned);
			e_.op(isSub ? OP_I64_SUB : OP_I64_ADD);
			e_.set(L_L0);
			putLoHi();
			return true;
		}
		case IROp::Div:
			// I0 = numerator, I1 = denominator
			Prepare({ S_LO, S_HI });
			get(SR(inst.src1)); e_.set(L_I0);
			get(SR(inst.src2)); e_.set(L_I1);
			e_.get(L_I0); e_.i32c(0x80000000); e_.op(OP_I32_EQ); e_.get(L_I1); e_.i32c(0xFFFFFFFF); e_.op(OP_I32_EQ); e_.op(OP_I32_AND);
			If();
				e_.i32c(0x80000000); put(S_LO);
				e_.i32c(0xFFFFFFFF); put(S_HI);
			Else();
				e_.get(L_I1);
				If();
					e_.get(L_I0); e_.get(L_I1); e_.op(OP_I32_DIV_S); put(S_LO);
					e_.get(L_I0); e_.get(L_I1); e_.op(OP_I32_REM_S); put(S_HI);
				Else();
					e_.i32c(1); e_.i32c(0xFFFFFFFF); e_.get(L_I0); e_.i32c(0); e_.op(OP_I32_LT_S); e_.op(OP_SELECT); put(S_LO);
					e_.get(L_I0); put(S_HI);
				End();
			End();
			return true;
		case IROp::DivU:
			Prepare({ S_LO, S_HI });
			get(SR(inst.src1)); e_.set(L_I0);
			get(SR(inst.src2)); e_.set(L_I1);
			e_.get(L_I1);
			If();
				e_.get(L_I0); e_.get(L_I1); e_.op(OP_I32_DIV_U); put(S_LO);
				e_.get(L_I0); e_.get(L_I1); e_.op(OP_I32_REM_U); put(S_HI);
			Else();
				e_.i32c(0xFFFF); e_.i32c(0xFFFFFFFF); e_.get(L_I0); e_.i32c(0xFFFF); e_.op(OP_I32_LE_U); e_.op(OP_SELECT); put(S_LO);
				e_.get(L_I0); put(S_HI);
			End();
			return true;
		case IROp::BSwap16:
			get(SR(inst.src1)); e_.set(L_I0);
			e_.get(L_I0); e_.i32c(0xFF00FF00); e_.op(OP_I32_AND); e_.i32c(8); e_.op(OP_I32_SHR_U);
			e_.get(L_I0); e_.i32c(0x00FF00FF); e_.op(OP_I32_AND); e_.i32c(8); e_.op(OP_I32_SHL);
			e_.op(OP_I32_OR); put(SR(inst.dest));
			return true;
		case IROp::BSwap32:
			get(SR(inst.src1)); e_.set(L_I0);
			e_.get(L_I0); e_.i32c(24); e_.op(OP_I32_SHR_U);
			e_.get(L_I0); e_.i32c(8); e_.op(OP_I32_SHR_U); e_.i32c(0xFF00); e_.op(OP_I32_AND); e_.op(OP_I32_OR);
			e_.get(L_I0); e_.i32c(8); e_.op(OP_I32_SHL); e_.i32c(0xFF0000); e_.op(OP_I32_AND); e_.op(OP_I32_OR);
			e_.get(L_I0); e_.i32c(24); e_.op(OP_I32_SHL); e_.op(OP_I32_OR);
			put(SR(inst.dest));
			return true;

		case IROp::FAdd: binF(inst, OP_F32_ADD); return true;
		case IROp::FSub: binF(inst, OP_F32_SUB); return true;
		case IROp::FDiv: binF(inst, OP_F32_DIV); return true;
		case IROp::FMul:
			// (b == 0 && isinf(a)) || (a == 0 && isinf(b)) ? 0x7fc00000 : a * b
			get(SF(inst.src1)); e_.set(L_I0);
			get(SF(inst.src2)); e_.set(L_I1);
			e_.i32c(0x7fc00000);
			e_.get(L_I0); e_.op(OP_F32_REINTERPRET_I32); e_.get(L_I1); e_.op(OP_F32_REINTERPRET_I32); e_.op(OP_F32_MUL); e_.op(OP_I32_REINTERPRET_F32);
			e_.get(L_I1); e_.op(OP_F32_REINTERPRET_I32); F32Const(0.0f); e_.op(OP_F32_EQ); isInfBits(L_I0); e_.op(OP_I32_AND);
			e_.get(L_I0); e_.op(OP_F32_REINTERPRET_I32); F32Const(0.0f); e_.op(OP_F32_EQ); isInfBits(L_I1); e_.op(OP_I32_AND);
			e_.op(OP_I32_OR);
			e_.op(OP_SELECT);
			put(SF(inst.dest));
			return true;
		case IROp::FMin:
		case IROp::FMax:
		{
			bool isMin = inst.op == IROp::FMin;
			get(SF(inst.src1)); e_.set(L_I0);
			get(SF(inst.src2)); e_.set(L_I1);
			// NaN case (vmin/vmax on the bits): both negative -> max (FMin) / min (FMax); else min (FMin) / max (FMax).
			e_.get(L_I0); e_.get(L_I1);
			e_.get(L_I0); e_.i32c(0); e_.op(OP_I32_LT_S); e_.get(L_I1); e_.i32c(0); e_.op(OP_I32_LT_S); e_.op(OP_I32_AND);
			If(T_I32);
				e_.get(L_I0); e_.get(L_I1); e_.op(isMin ? OP_I32_GT_S : OP_I32_LT_S);
			Else();
				e_.get(L_I0); e_.get(L_I1); e_.op(isMin ? OP_I32_LT_S : OP_I32_GT_S);
			End();
			e_.op(OP_SELECT);
			e_.set(L_I2);
			// Ordered case: std::min(a, b) = b < a ? b : a; std::max(a, b) = a < b ? b : a
			e_.get(L_I1); e_.get(L_I0);
			if (isMin) {
				e_.get(L_I1); e_.op(OP_F32_REINTERPRET_I32); e_.get(L_I0); e_.op(OP_F32_REINTERPRET_I32);
			} else {
				e_.get(L_I0); e_.op(OP_F32_REINTERPRET_I32); e_.get(L_I1); e_.op(OP_F32_REINTERPRET_I32);
			}
			e_.op(OP_F32_LT);
			e_.op(OP_SELECT);
			e_.set(L_I3);
			e_.get(L_I2); e_.get(L_I3);
			isNanBits(L_I0); isNanBits(L_I1); e_.op(OP_I32_OR);
			e_.op(OP_SELECT);
			put(SF(inst.dest));
			return true;
		}
		case IROp::FMov: get(SF(inst.src1)); put(SF(inst.dest)); return true;
		case IROp::FAbs: unF(inst, OP_F32_ABS); return true;
		case IROp::FSqrt: unF(inst, OP_F32_SQRT); return true;
		case IROp::FNeg: unF(inst, OP_F32_NEG); return true;
		case IROp::FSign:
			get(SF(inst.src1)); e_.set(L_I0);
			// val == 0 || val == 0x80000000 ? 0 : (val >> 31 ? -1 : 1)
			e_.i32c(0xBF800000); e_.i32c(0x3F800000); e_.get(L_I0); e_.i32c(31); e_.op(OP_I32_SHR_U); e_.op(OP_SELECT);
			e_.i32c(0);
			e_.get(L_I0); e_.i32c(0x7FFFFFFF); e_.op(OP_I32_AND);
			e_.op(OP_SELECT);
			put(SF(inst.dest));
			return true;
		case IROp::FpCondFromReg: get(SR(inst.dest)); put(S_FPCOND); return true;
		case IROp::FpCondToReg: get(S_FPCOND); put(SR(inst.dest)); return true;
		case IROp::FpCtrlFromReg:
			get(SR(inst.src1)); e_.i32c(0x0181FFFF); e_.op(OP_I32_AND); put(S_FCR31);
			get(S_FCR31); e_.i32c(23); e_.op(OP_I32_SHR_U); e_.i32c(1); e_.op(OP_I32_AND); put(S_FPCOND);
			return true;
		case IROp::FpCtrlToReg:
			get(S_FCR31); e_.i32c(~(1u << 23)); e_.op(OP_I32_AND);
			get(S_FPCOND); e_.i32c(1); e_.op(OP_I32_AND); e_.i32c(23); e_.op(OP_I32_SHL);
			e_.op(OP_I32_OR); put(S_FCR31);
			get(S_FCR31); put(SR(inst.dest));
			return true;
		case IROp::VfpuCtrlToReg: get(SV(inst.src1)); put(SR(inst.dest)); return true;
		case IROp::SetCtrlVFPU: e_.i32c(inst.constant); put(SV(inst.dest)); return true;
		case IROp::SetCtrlVFPUReg: get(SR(inst.src1)); put(SV(inst.dest)); return true;
		case IROp::SetCtrlVFPUFReg: get(SF(inst.src1)); put(SV(inst.dest)); return true;
		case IROp::FCmp:
			switch (inst.dest) {
			case IRFpCompareMode::False: e_.i32c(0); break;
			case IRFpCompareMode::EitherUnordered:
				// !(a > b || a < b || a == b)
				getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_GT);
				getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_LT); e_.op(OP_I32_OR);
				getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_EQ); e_.op(OP_I32_OR);
				e_.op(OP_I32_EQZ);
				break;
			case IRFpCompareMode::EqualOrdered: getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_EQ); break;
			case IRFpCompareMode::EqualUnordered:
				get(SF(inst.src1)); e_.set(L_I0);
				get(SF(inst.src2)); e_.set(L_I1);
				getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_EQ);
				isNanBits(L_I0); e_.op(OP_I32_OR);
				isNanBits(L_I1); e_.op(OP_I32_OR);
				break;
			case IRFpCompareMode::LessEqualOrdered: getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_LE); break;
			case IRFpCompareMode::LessEqualUnordered: getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_GT); e_.op(OP_I32_EQZ); break;
			case IRFpCompareMode::LessOrdered: getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_LT); break;
			case IRFpCompareMode::LessUnordered: getF(SF(inst.src1)); getF(SF(inst.src2)); e_.op(OP_F32_GE); e_.op(OP_I32_EQZ); break;
			default: return false;
			}
			put(S_FPCOND);
			return true;
		case IROp::FCvtSW: get(SF(inst.src1)); e_.op(OP_F32_CONVERT_I32_S); putF(SF(inst.dest)); return true;
		case IROp::FCvtScaledSW:
			get(SF(inst.src1)); e_.op(OP_F32_CONVERT_I32_S);
			F32Const(1.0f / (1UL << (inst.src2 & 0x1F))); e_.op(OP_F32_MUL); putF(SF(inst.dest));
			return true;
		case IROp::FMovFromGPR: get(SR(inst.src1)); put(SF(inst.dest)); return true;
		case IROp::OptFCvtSWFromGPR: get(SR(inst.src1)); e_.op(OP_F32_CONVERT_I32_S); putF(SF(inst.dest)); return true;
		case IROp::FMovToGPR: get(SF(inst.src1)); put(SR(inst.dest)); return true;
		case IROp::OptFMovToGPRShr8: get(SF(inst.src1)); e_.i32c(8); e_.op(OP_I32_SHR_U); put(SR(inst.dest)); return true;

		case IROp::ExitToConst: exitTo(inst.constant); endsWithExit_ = true; return true;
		case IROp::ExitToReg: Flush(); get(SR(inst.src1)); e_.op(OP_RETURN); endsWithExit_ = true; return true;
		case IROp::ExitToPC: Flush(); get(S_PC); e_.op(OP_RETURN); endsWithExit_ = true; return true;
		case IROp::ExitToConstIfEq: exitIf(inst, OP_I32_EQ, false); return true;
		case IROp::ExitToConstIfNeq: exitIf(inst, OP_I32_NE, false); return true;
		case IROp::ExitToConstIfGtZ: exitIf(inst, OP_I32_GT_S, true); return true;
		case IROp::ExitToConstIfGeZ: exitIf(inst, OP_I32_GE_S, true); return true;
		case IROp::ExitToConstIfLtZ: exitIf(inst, OP_I32_LT_S, true); return true;
		case IROp::ExitToConstIfLeZ: exitIf(inst, OP_I32_LE_S, true); return true;
		case IROp::Downcount: get(S_DOWNCOUNT); e_.i32c(inst.constant); e_.op(OP_I32_SUB); put(S_DOWNCOUNT); return true;
		case IROp::SetPC: get(SR(inst.src1)); put(S_PC); return true;
		case IROp::SetPCConst: e_.i32c(inst.constant); put(S_PC); return true;
		case IROp::UpdateRoundingMode: return true;

		default:
			return false;
		}
	}
};

void WriteSection(Emitter &out, uint8_t id, const Emitter &content) {
	out.u8(id);
	out.uleb((uint32_t)content.b.size());
	out.bytes(content.b);
}

}  // namespace

void IRWasmJit::Reset() {
	entries_.clear();
	pending_.clear();
	for (int slot : usedSlots_)
		freeSlots_.push_back(slot);
	usedSlots_.clear();
	stats[7] = 0;
}

void IRWasmJit::BeginSlice(IRBlockCache &blocks) {
	if (mode != lastMode_) {
		// Switching modes (or the first slice): start from a clean slate.
		lastMode_ = mode;
		Reset();
	}
	if (mode == 0)
		return;
	int token = irwasm_token();
	if (token != token_) {
		// New thread/worker: its function table doesn't have our slots.
		token_ = token;
		entries_.clear();
		pending_.clear();
		usedSlots_.clear();
		freeSlots_.clear();
		pure_.clear();
	}
	size_t arenaSize = blocks.GetArenaSize();
	if (entries_.size() < arenaSize)
		entries_.resize(arenaSize + 4096, 0);
	slicesSinceFlush_++;
	if (pending_.size() >= 64 || (!pending_.empty() && slicesSinceFlush_ > 100))
		Flush(blocks);
}

void IRWasmJit::Flush(IRBlockCache &blocks) {
	slicesSinceFlush_ = 0;
	const IRInst *arena = blocks.GetArenaPtr();
	const uint32_t memBase = (uint32_t)(uintptr_t)Memory::base;
	const uint32_t interpIdx = (uint32_t)(uintptr_t)&IRInterpret;
	const uint32_t journal = mode == 2 ? (uint32_t)(uintptr_t)g_journal : 0;

	// Blocks of this batch, [offset, end) in the arena. Each function starts at the block's first op (its Downcount).
	struct Item { u32 offset, end; int slot; uint8_t pure; };
	std::vector<Item> items;
	for (u32 offset : pending_) {
		int blockNum = blocks.GetBlockNumFromIRArenaOffset((int)offset);
		const IRBlock *block = blocks.GetBlock(blockNum);
		if (!block || !block->IsValid())
			continue;
		items.push_back({ offset, offset + (u32)block->GetNumIRInstructions(), 0, 0 });
	}
	pending_.clear();

	// Pass 1: which blocks compile at all (links may only point at functions that will exist).
	std::vector<Item> ok;
	int savedStats[8];
	memcpy(savedStats, stats, sizeof(stats));
	for (const Item &it : items) {
		Emitter fn;
		BlockCompiler bc(fn, memBase, interpIdx, journal, BlockCompiler::LinkResolver());
		bool isPure = false;
		if (bc.Compile(arena, it.offset, it.end, &isPure))
			ok.push_back(it);
	}
	memcpy(stats, savedStats, sizeof(stats));  // counted in pass 2
	if (ok.empty())
		return;
	const uint32_t n = (uint32_t)ok.size();

	// Table slots up front, so blocks of the same batch can link to each other.
	uint32_t fresh = 0;
	for (Item &it : ok) {
		if (!freeSlots_.empty()) {
			it.slot = freeSlots_.back();
			freeSlots_.pop_back();
		} else {
			fresh++;
		}
	}
	if (fresh) {
		int base = irwasm_reserve((int)fresh);
		if (base <= 0) {
			stats[2]++;
			for (Item &it : ok)
				if (it.slot) freeSlots_.push_back(it.slot);
			return;
		}
		for (Item &it : ok)
			if (!it.slot) it.slot = base++;
	}
	std::unordered_map<u32, int> batch;
	for (const Item &it : ok)
		batch[it.offset] = it.slot;

	// Links only in normal mode: verify mode checks one block at a time.
	BlockCompiler::LinkResolver resolver;
	if (mode == 1) {
		resolver = [&](u32 target, u32 *word) -> int {
			u32 w = Memory::ReadUnchecked_U32(target);
			if ((w & 0xFF000000) != MIPS_EMUHACK_OPCODE)
				return 0;
			u32 off = w & 0x00FFFFFF;
			int slot = 0;
			if (off < entries_.size() && entries_[off] > 0) {
				slot = entries_[off];
			} else {
				auto f = batch.find(off);
				if (f != batch.end())
					slot = f->second;
			}
			*word = w;
			return slot;
		};
	}

	// Pass 2: the real code.
	Emitter code;
	for (Item &it : ok) {
		Emitter fn;
		BlockCompiler bc(fn, memBase, interpIdx, journal, resolver);
		bool isPure = false;
		bc.Compile(arena, it.offset, it.end, &isPure);
		it.pure = isPure ? 1 : 0;
		code.bytes(fn.b);
	}

	Emitter mod;
	const uint8_t header[] = { 0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00 };
	mod.b.assign(header, header + sizeof(header));
	{
		Emitter s;  // type 0: (i32, i32) -> i32, same as IRInterpret
		s.uleb(1); s.u8(0x60); s.uleb(2); s.u8(T_I32); s.u8(T_I32); s.uleb(1); s.u8(T_I32);
		WriteSection(mod, 1, s);
	}
	{
		Emitter s;  // imports: shared memory e.m, table e.t
		s.uleb(2);
		s.str("e"); s.str("m"); s.u8(0x02); s.u8(0x03); s.uleb(0); s.uleb(65536);
		s.str("e"); s.str("t"); s.u8(0x01); s.u8(0x70); s.u8(0x00); s.uleb(0);
		WriteSection(mod, 2, s);
	}
	{
		Emitter s;
		s.uleb(n);
		for (uint32_t i = 0; i < n; i++) s.uleb(0);
		WriteSection(mod, 3, s);
	}
	{
		Emitter s;
		s.uleb(n);
		char name[16];
		for (uint32_t i = 0; i < n; i++) {
			snprintf(name, sizeof(name), "f%u", i);
			s.str(name); s.u8(0x00); s.uleb(i);
		}
		WriteSection(mod, 7, s);
	}
	{
		Emitter s;
		s.uleb(n);
		s.bytes(code.b);
		WriteSection(mod, 10, s);
	}

	std::vector<int> slots(n);
	for (uint32_t i = 0; i < n; i++)
		slots[i] = ok[i].slot;
	if (!irwasm_install(mod.b.data(), (int)mod.b.size(), slots.data(), (int)n)) {
		stats[2]++;
		// Only this batch links to these slots: give them back, the blocks stay interpreted.
		for (int slot : slots)
			freeSlots_.push_back(slot);
		return;
	}
	for (uint32_t i = 0; i < n; i++) {
		if (ok[i].offset < entries_.size())
			entries_[ok[i].offset] = slots[i];
		usedSlots_.push_back(slots[i]);
		if ((size_t)slots[i] >= pure_.size())
			pure_.resize(slots[i] + 1024, 0);
		pure_[slots[i]] = ok[i].pure;
	}
	stats[0] += n;
	stats[7] = (int)usedSlots_.size();
}

u32 IRWasmJit::Run(int func, MIPSState *mips, const IRInst *arena, u32 offset) {
	if (mode != 2)
		return ((IRWasmBlockFunc)(uintptr_t)func)(mips, arena);
	// Verify mode compiles every block with the store journal: start it empty on every run.
	*(u32 *)g_journal = 0;
	if ((size_t)func >= pure_.size() || !pure_[func])
		return ((IRWasmBlockFunc)(uintptr_t)func)(mips, arena);

	// Verify: same start state through both, compare everything the block can touch in MIPSState.
	// Everything up to debugCount: registers, pc, downcount, llBit... (the block can't touch the rest).
	const size_t size = offsetof(MIPSState, debugCount);
	alignas(16) static uint8_t before[2048], afterJit[2048];
	static_assert(offsetof(MIPSState, debugCount) <= 2048, "verify buffers");
	memcpy(before, (const void *)mips, size);
	const IRInst *start = arena + offset;
	u32 *journalCount = (u32 *)g_journal;
	*journalCount = 0;
	u32 pcJit = ((IRWasmBlockFunc)(uintptr_t)func)(mips, arena);
	memcpy(afterJit, (const void *)mips, size);
	// Keep what the JIT wrote, then undo its stores newest first.
	const u32 stores = *journalCount;
	for (u32 k = 0; k < stores; k++) {
		u32 a;
		memcpy(&a, g_journal + 16 + k * JOURNAL_ENTRY, 4);
		memcpy(g_journalNew + 16 * k, Memory::base + a, 16);
	}
	for (u32 k = stores; k-- > 0;) {
		u32 a;
		memcpy(&a, g_journal + 16 + k * JOURNAL_ENTRY, 4);
		memcpy(Memory::base + a, g_journal + 16 + k * JOURNAL_ENTRY + 16, 16);
	}
	memcpy((void *)mips, before, size);
	// The interpreter path of the dispatcher: the block's first Downcount in C, then IRInterpret.
	if (start->op == IROp::Downcount) {
		mips->downcount -= start->constant;
		start++;
	}
	u32 pcInterp = IRInterpret(mips, start);
	stats[3]++;
	bool memOk = true;
	for (u32 k = 0; k < stores && memOk; k++) {
		u32 a;
		memcpy(&a, g_journal + 16 + k * JOURNAL_ENTRY, 4);
		if (memcmp(g_journalNew + 16 * k, Memory::base + a, 16) != 0) {
			memOk = false;
			emscripten_log(EM_LOG_ERROR, "IRWasmJit verify: arena %u memory differs at %08x (store %u of %u)", offset, a, k, stores);
		}
	}
	if (!memOk || pcJit != pcInterp || memcmp(afterJit, (const void *)mips, size) != 0) {
		stats[4]++;
		if (stats[4] <= 20) {
			const u32 *a = (const u32 *)afterJit;
			const u32 *b = (const u32 *)mips;
			for (size_t w = 0; w < size / 4; w++) {
				if (a[w] != b[w]) {
					ERROR_LOG(Log::JIT, "IRWasmJit verify: block %08x word %d jit %08x interp %08x (op %d)",
						mips->pc, (int)w, a[w], b[w], (int)start->op);
					emscripten_log(EM_LOG_ERROR, "IRWasmJit verify: arena %u word %d jit %08x interp %08x pc %08x/%08x",
						offset, (int)w, a[w], b[w], pcJit, pcInterp);
					break;
				}
			}
			if (pcJit != pcInterp)
				emscripten_log(EM_LOG_ERROR, "IRWasmJit verify: arena %u pc jit %08x interp %08x", offset, pcJit, pcInterp);
		}
		// Stop using this block.
		entries_[offset] = NEVER;
	}
	return pcInterp;
}

}  // namespace MIPSComp

extern "C" __attribute__((used)) void ejs_wasmjit_mode(int m) {
	MIPSComp::IRWasmJit::mode = m;
}

extern "C" __attribute__((used)) int ejs_wasmjit_stat(int i) {
	return i >= 0 && i < 8 ? MIPSComp::IRWasmJit::stats[i] : 0;
}

#endif
