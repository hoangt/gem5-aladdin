/*
 * Copyright (c) 2007 The Hewlett-Packard Development Company
 * All rights reserved.
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Gabe Black
 */

#ifndef __ARCH_X86_PREDECODER_HH__
#define __ARCH_X86_PREDECODER_HH__

#include <cassert>

#include "arch/x86/types.hh"
#include "arch/x86/regs/misc.hh"
#include "base/bitfield.hh"
#include "base/misc.hh"
#include "base/trace.hh"
#include "base/types.hh"

class ThreadContext;

namespace X86ISA
{
    class Predecoder
    {
      private:
        //These are defined and documented in predecoder_tables.cc
        static const uint8_t Prefixes[256];
        static const uint8_t UsesModRM[2][256];
        static const uint8_t ImmediateType[2][256];
        static const uint8_t SizeTypeToSize[3][10];

      protected:
        ThreadContext * tc;
        //The bytes to be predecoded
        MachInst fetchChunk;
        //The pc of the start of fetchChunk
        Addr basePC;
        //The pc the current instruction started at
        Addr origPC;
        //The offset into fetchChunk of current processing
        int offset;
        //The extended machine instruction being generated
        ExtMachInst emi;
        HandyM5Reg m5Reg;

        inline uint8_t getNextByte()
        {
            return ((uint8_t *)&fetchChunk)[offset];
        }

        void getImmediate(int &collected, uint64_t &current, int size)
        {
            //Figure out how many bytes we still need to get for the
            //immediate.
            int toGet = size - collected;
            //Figure out how many bytes are left in our "buffer"
            int remaining = sizeof(MachInst) - offset;
            //Get as much as we need, up to the amount available.
            toGet = toGet > remaining ? remaining : toGet;

            //Shift the bytes we want to be all the way to the right
            uint64_t partialImm = fetchChunk >> (offset * 8);
            //Mask off what we don't want
            partialImm &= mask(toGet * 8);
            //Shift it over to overlay with our displacement.
            partialImm <<= (immediateCollected * 8);
            //Put it into our displacement
            current |= partialImm;
            //Update how many bytes we've collected.
            collected += toGet;
            consumeBytes(toGet);
        }

        inline void consumeByte()
        {
            offset++;
            assert(offset <= sizeof(MachInst));
            if(offset == sizeof(MachInst))
                outOfBytes = true;
        }

        inline void consumeBytes(int numBytes)
        {
            offset += numBytes;
            assert(offset <= sizeof(MachInst));
            if(offset == sizeof(MachInst))
                outOfBytes = true;
        }

        void doReset();

        //State machine state
      protected:
        //Whether or not we're out of bytes
        bool outOfBytes;
        //Whether we've completed generating an ExtMachInst
        bool emiIsReady;
        //The size of the displacement value
        int displacementSize;
        //The size of the immediate value
        int immediateSize;
        //This is how much of any immediate value we've gotten. This is used
        //for both the actual immediate and the displacement.
        int immediateCollected;

        enum State {
            ResetState,
            PrefixState,
            OpcodeState,
            ModRMState,
            SIBState,
            DisplacementState,
            ImmediateState,
            //We should never get to this state. Getting here is an error.
            ErrorState
        };

        State state;

        //Functions to handle each of the states
        State doPrefixState(uint8_t);
        State doOpcodeState(uint8_t);
        State doModRMState(uint8_t);
        State doSIBState(uint8_t);
        State doDisplacementState();
        State doImmediateState();

      public:
        Predecoder(ThreadContext * _tc) :
            tc(_tc), basePC(0), origPC(0), offset(0),
            outOfBytes(true), emiIsReady(false),
            state(ResetState)
        {
            emi.mode.mode = LongMode;
            emi.mode.submode = SixtyFourBitMode;
            m5Reg = 0;
        }

        void reset()
        {
            state = ResetState;
        }

        void
        reset(const ExtMachInst &old_emi)
        {
            reset();
        }

        ThreadContext * getTC()
        {
            return tc;
        }

        void setTC(ThreadContext * _tc)
        {
            tc = _tc;
        }

        void process();

        //Use this to give data to the predecoder. This should be used
        //when there is control flow.
        void moreBytes(const PCState &pc, Addr fetchPC, MachInst data)
        {
            DPRINTF(Predecoder, "Getting more bytes.\n");
            basePC = fetchPC;
            offset = (fetchPC >= pc.instAddr()) ? 0 : pc.instAddr() - fetchPC;
            fetchChunk = data;
            outOfBytes = false;
            process();
        }

        bool needMoreBytes()
        {
            return outOfBytes;
        }

        bool extMachInstReady()
        {
            return emiIsReady;
        }

        int
        getInstSize()
        {
            int size = basePC + offset - origPC;
            DPRINTF(Predecoder,
                    "Calculating the instruction size: "
                    "basePC: %#x offset: %#x origPC: %#x size: %d\n",
                    basePC, offset, origPC, size);
            return size;
        }

        //This returns a constant reference to the ExtMachInst to avoid a copy
        const ExtMachInst &
        getExtMachInst(X86ISA::PCState &nextPC)
        {
            assert(emiIsReady);
            emiIsReady = false;
            if (!nextPC.size()) {
                Addr size = getInstSize();
                nextPC.size(size);
                nextPC.npc(nextPC.pc() + size);
            }
            return emi;
        }
    };
};

#endif // __ARCH_X86_PREDECODER_HH__
