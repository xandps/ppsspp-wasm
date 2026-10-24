#pragma once

// IR -> WebAssembly backend for the Emscripten build (arcavive-psp, P2).
//
// Hot IR blocks are translated into WebAssembly functions with the same signature and semantics as
// IRInterpret(MIPSState *, const IRInst *): they take the MIPS state and the IR arena base, run the block and
// return the next PC. Batches of blocks become one module, compiled synchronously on the emulation thread and
// installed in that thread's function table, so the dispatcher can call them through a plain function pointer.
//
// Ops without a native translation call IRInterpret from that instruction on, so a block is always correct even
// when only a prefix of it is native. Mode 2 (verify) runs pure blocks (no stores, no fallbacks) through both and
// compares the resulting state.

#ifdef __EMSCRIPTEN__

#include <cstdint>
#include <vector>

#include "Common/CommonTypes.h"

struct IRInst;
class MIPSState;

namespace MIPSComp {

class IRBlockCache;

typedef u32 (*IRWasmBlockFunc)(MIPSState *mips, const IRInst *arena);

class IRWasmJit {
public:
	// Called once per CoreTiming slice from the dispatcher: checks the thread and compiles queued blocks.
	void BeginSlice(IRBlockCache &blocks);
	// Forget all compiled code (IR arena cleared).
	void Reset();

	// Table index of the compiled block at this arena offset, or <= 0 when not compiled (counts the execution).
	inline int Lookup(u32 offset) {
		if (offset >= entries_.size())
			return 0;
		int32_t &e = entries_[offset];
		if (e > 0)
			return e;
		if (e > -THRESHOLD) {
			e--;
		} else if (e == -THRESHOLD) {
			e = NEVER;
			pending_.push_back(offset);
		}
		return 0;
	}

	u32 Run(int func, MIPSState *mips, const IRInst *arena, u32 offset);

	static int mode;  // 0 = off (IR interpreter only), 1 = on, 2 = on + verify pure blocks
	static int stats[8];

private:
	static constexpr int THRESHOLD = 50;
	static constexpr int32_t NEVER = INT32_MIN;

	void Flush(IRBlockCache &blocks);

	std::vector<int32_t> entries_;     // by IR arena offset: >0 table index, <=0 -executions, NEVER
	std::vector<u32> pending_;         // arena offsets waiting for compilation
	std::vector<int> usedSlots_;       // table slots holding our functions (this thread's table)
	std::vector<int> freeSlots_;
	std::vector<uint8_t> pure_;        // by table slot: verifiable in mode 2
	int token_ = 0;
	int slicesSinceFlush_ = 0;
	int lastMode_ = -1;
};

}  // namespace MIPSComp

#endif
