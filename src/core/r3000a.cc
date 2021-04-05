/***************************************************************************
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.           *
 ***************************************************************************/

/*
 * R3000A CPU functions.
 */

#include "core/r3000a.h"

#include "core/cdrom.h"
#include "core/debug.h"
#include "core/gpu.h"
#include "core/gte.h"
#include "core/mdec.h"
#include "core/pgxp_mem.h"
#include "core/spu.h"
#include "fmt/format.h"
#include "magic_enum/include/magic_enum.hpp"

int PCSX::R3000Acpu::psxInit() {
    g_system->printf(_("PCSX-Redux booting\n"));
    g_system->printf(_("Copyright (C) 2019-2021 PCSX-Redux authors\n"));

    if (g_emulator->settings.get<Emulator::SettingDynarec>()) g_emulator->m_psxCpu = Cpus::DynaRec();
    if (!g_emulator->m_psxCpu) g_emulator->m_psxCpu = Cpus::Interpreted();

    PGXP_Init();

    return g_emulator->m_psxCpu->Init();
}

void PCSX::R3000Acpu::psxReset() {
    Reset();

    memset(&m_psxRegs, 0, sizeof(m_psxRegs));
    m_shellStarted = false;

    m_psxRegs.pc = 0xbfc00000;  // Start in bootstrap

    m_psxRegs.CP0.r[12] = 0x10900000;  // COP0 enabled | BEV = 1 | TS = 1
    m_psxRegs.CP0.r[15] = 0x00000002;  // PRevID = Revision ID, same as R3000A

    PCSX::g_emulator->m_hw->psxHwReset();
}

void PCSX::R3000Acpu::psxShutdown() { Shutdown(); }

void PCSX::R3000Acpu::psxException(uint32_t code, bool bd) {
    // Set the Cause
    unsigned ec = (code >> 2) & 0x1f;
    auto e = magic_enum::enum_cast<Exception>(ec);
    if (e.has_value()) {
        ec = 1 << ec;
        if (g_emulator->settings.get<Emulator::SettingFirstChanceException>() & ec) {
            auto name = magic_enum::enum_name(e.value());
            g_system->printf(fmt::format("First chance exception: {} from 0x{:08x}\n", name, m_psxRegs.pc).c_str());
            g_system->pause();
        }
    }

    m_inISR = true;

    // Set the EPC & PC
    if (bd) {
        code |= 0x80000000;
        m_psxRegs.CP0.n.EPC = (m_psxRegs.pc - 4);
    } else {
        m_psxRegs.CP0.n.EPC = (m_psxRegs.pc);
    }

    if (m_psxRegs.CP0.n.Status & 0x400000) {
        m_psxRegs.pc = 0xbfc00180;
    } else {
        m_psxRegs.pc = 0x80000080;
    }

    m_psxRegs.CP0.n.Cause = code;
    // Set the Status
    m_psxRegs.CP0.n.Status = (m_psxRegs.CP0.n.Status & ~0x3f) | ((m_psxRegs.CP0.n.Status & 0xf) << 2);
}

void PCSX::R3000Acpu::psxBranchTest() {
#if 0
    if( SPU_async )
    {
        static int init;
        int elapsed;

        if( init == 0 ) {
            // 10 apu cycles
            // - Final Fantasy Tactics (distorted - dropped sound effects)
            m_psxRegs.intCycle[PSXINT_SPUASYNC].cycle = g_emulator->m_psxClockSpeed / 44100 * 10;

            init = 1;
        }

        elapsed = m_psxRegs.cycle - m_psxRegs.intCycle[PSXINT_SPUASYNC].sCycle;
        if (elapsed >= m_psxRegs.intCycle[PSXINT_SPUASYNC].cycle) {
            SPU_async( elapsed );

            m_psxRegs.intCycle[PSXINT_SPUASYNC].sCycle = m_psxRegs.cycle;
        }
    }
#endif

    const uint32_t cycle = m_psxRegs.cycle;

    if ((cycle - PCSX::g_emulator->m_psxCounters->m_psxNextsCounter) >=
        PCSX::g_emulator->m_psxCounters->m_psxNextCounter)
        PCSX::g_emulator->m_psxCounters->psxRcntUpdate();

    if (m_psxRegs.spuInterrupt.exchange(false)) PCSX::g_emulator->m_spu->interrupt();

    const uint32_t interrupts = m_psxRegs.interrupt;

    int32_t lowestDistance = std::numeric_limits<int32_t>::max();
    uint32_t lowestTarget = cycle;
    uint32_t *targets = m_psxRegs.intTargets;

    if ((interrupts != 0) && (((int32_t)(m_psxRegs.lowestTarget - cycle)) <= 0)) {
        auto checkAndUpdate = [&lowestDistance, &lowestTarget, interrupts, cycle, targets, this](
                                  unsigned interrupt, std::function<void()> act) {
            uint32_t mask = 1 << interrupt;
            if ((interrupts & mask) == 0) return;
            uint32_t target = targets[interrupt];
            int32_t dist = target - cycle;
            if (dist > 0) {
                if (lowestDistance > dist) {
                    lowestDistance = dist;
                    lowestTarget = target;
                }
            } else {
                m_psxRegs.interrupt &= ~mask;
                PSXIRQ_LOG("Triggering interrupt %08x\n", interrupt);
                act();
            }
        };

        checkAndUpdate(PSXINT_SIO, []() { g_emulator->m_sio->interrupt(); });
        checkAndUpdate(PSXINT_CDR, []() { g_emulator->m_cdrom->interrupt(); });
        checkAndUpdate(PSXINT_CDREAD, []() { g_emulator->m_cdrom->readInterrupt(); });
        checkAndUpdate(PSXINT_GPUDMA, []() { GPU::gpuInterrupt(); });
        checkAndUpdate(PSXINT_MDECOUTDMA, []() { g_emulator->m_mdec->mdec1Interrupt(); });
        checkAndUpdate(PSXINT_SPUDMA, []() { spuInterrupt(); });
        checkAndUpdate(PSXINT_MDECINDMA, []() { g_emulator->m_mdec->mdec0Interrupt(); });
        checkAndUpdate(PSXINT_GPUOTCDMA, []() { gpuotcInterrupt(); });
        checkAndUpdate(PSXINT_CDRDMA, []() { g_emulator->m_cdrom->dmaInterrupt(); });
        checkAndUpdate(PSXINT_CDRPLAY, []() { g_emulator->m_cdrom->playInterrupt(); });
        checkAndUpdate(PSXINT_CDRDBUF, []() { g_emulator->m_cdrom->decodedBufferInterrupt(); });
        checkAndUpdate(PSXINT_CDRLID, []() { g_emulator->m_cdrom->lidSeekInterrupt(); });
        m_psxRegs.lowestTarget = lowestTarget;
    }
    if ((psxHu32(0x1070) & psxHu32(0x1074)) && ((m_psxRegs.CP0.n.Status & 0x401) == 0x401)) {
        PSXIRQ_LOG("Interrupt: %x %x\n", psxHu32(0x1070), psxHu32(0x1074));
        psxException(0x400, 0);
    }
}

void PCSX::R3000Acpu::psxSetPGXPMode(uint32_t pgxpMode) {
    SetPGXPMode(pgxpMode);
    // g_emulator->m_psxCpu->Reset();
}

static std::string fileFlagsToString(uint16_t flags) {
    std::string ret = " ";
    if (flags & 0x0001) ret += "READ ";
    if (flags & 0x0002) ret += "WRITE ";
    if (flags & 0x0004) ret += "NBLOCK ";
    if (flags & 0x0008) ret += "SCAN ";
    if (flags & 0x0010) ret += "RLOCK ";
    if (flags & 0x0020) ret += "WLOCK ";
    if (flags & 0x0040) ret += "U0040 ";
    if (flags & 0x0080) ret += "U0080 ";
    if (flags & 0x0100) ret += "APPEND ";
    if (flags & 0x0200) ret += "CREAT ";
    if (flags & 0x0400) ret += "TRUNC ";
    if (flags & 0x0800) ret += "U0800 ";
    if (flags & 0x1000) ret += "SCAN2 ";
    if (flags & 0x2000) ret += "RCOM ";
    if (flags & 0x4000) ret += "NBUF ";
    if (flags & 0x8000) ret += "ASYNC ";
    return ret;
}

void PCSX::R3000Acpu::logA0KernelCall(uint32_t call) {
    auto &n = m_psxRegs.GPR.n;
    switch (call) {
        case 0x00: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:open(\"%s\", 0x%04x {%s}) from 0x%08x\n", call,
                          PSXM(n.a0), n.a1, fileFlagsToString(n.a1), n.ra);
            break;
        }
        case 0x01: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:lseek(%i, %i, %i) from 0x%08x\n", call, n.a0, n.a1,
                          n.a2, n.ra);
            break;
        }
        case 0x02: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:read(%i, 0x%08x, %i) from 0x%08x\n", call, n.a0, n.a1,
                          n.a2, n.ra);
            break;
        }
        case 0x03: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:write(%i, 0x%08x, %i) from 0x%08x\n", call, n.a0, n.a1,
                          n.a2, n.ra);
            break;
        }
        case 0x04: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:close(%i) from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x05: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:ioctl(%i, %i, %i) from 0x%08x\n", call, n.a0, n.a1,
                          n.a2, n.ra);
            break;
        }
        case 0x06: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:exit(%i) from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x07: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:isFileConsole(%i) from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x08: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:getc(%i) from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x09: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:putc(%i, %i) from 0x%08x\n", call, n.a0, n.a1, n.ra);
            break;
        }
        case 0x0a: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:todigit(%i) from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x0c: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strtoul(\"%s\", call, 0x%08x, %i) from 0x%08x\n", call,
                          PSXM(n.a0), n.a1, n.a2, n.ra);
            break;
        }
        case 0x0d: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strtol(\"%s\", call, 0x%08x, %i) from 0x%08x\n", call,
                          PSXM(n.a0), n.a1, n.a2, n.ra);
            break;
        }
        case 0x0e: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:abs(%i) from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x0f: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:labs(%i) from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x10: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:atoi(\"%s\") from 0x%08x\n", call, PSXM(n.a0), n.ra);
            break;
        }
        case 0x11: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:atol(\"%s\") from 0x%08x\n", call, PSXM(n.a0), n.ra);
            break;
        }
        case 0x12: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:atob(\"%s\", 0x%08x) from 0x%08x\n", call, PSXM(n.a0),
                          n.a1, n.ra);
            break;
        }
        case 0x13: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:setjmp(0x%08x) from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x14: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:longjmp(0x%08x, %i) from 0x%08x\n", call, n.a0, n.a1,
                          n.ra);
            break;
        }
        case 0x15: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strcat(\"%s\", \"%s\") from 0x%08x\n", call, PSXM(n.a0),
                          PSXM(n.a1), n.ra);
            break;
        }
        case 0x16: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strncat(\"%s\", \"%s\", %i) from 0x%08x\n", call,
                          PSXM(n.a0), PSXM(n.a1), n.a2, n.ra);
            break;
        }
        case 0x17: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strcmp(\"%s\", \"%s\") from 0x%08x\n", call, PSXM(n.a0),
                          PSXM(n.a1), n.ra);
            break;
        }
        case 0x18: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strncmp(\"%s\", \"%s\", %i) from 0x%08x\n", call,
                          PSXM(n.a0), PSXM(n.a1), n.a2, n.ra);
            break;
        }
        case 0x19: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strcpy(0x%08x, \"%s\") from 0x%08x\n", call, n.a0,
                          PSXM(n.a1), n.ra);
            break;
        }
        case 0x1a: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strncpy(0x%08x, \"%s\", %i) from 0x%08x\n", call, n.a0,
                          PSXM(n.a1), n.a2, n.ra);
            break;
        }
        case 0x1b: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strlen(0x%08x) from 0x%08x\n", call, PSXM(n.a0), n.ra);
            break;
        }
        case 0x1c: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:index(\"%s\", '%c') from 0x%08x\n", call, PSXM(n.a0),
                          n.a1, n.ra);
            break;
        }
        case 0x1d: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:rindex(\"%s\", '%c') from 0x%08x\n", call, PSXM(n.a0),
                          n.a1, n.ra);
            break;
        }
        case 0x1e: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strchr(\"%s\", '%c') from 0x%08x\n", call, PSXM(n.a0),
                          n.a1, n.ra);
            break;
        }
        case 0x1f: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strrchr(\"%s\", '%c') from 0x%08x\n", call, PSXM(n.a0),
                          n.a1, n.ra);
            break;
        }
        case 0x20: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strpbrk(\"%s\", \"%s\") from 0x%08x\n", call,
                          PSXM(n.a0), PSXM(n.a1), n.ra);
            break;
        }
        case 0x21: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strspn(\"%s\", \"%s\") from 0x%08x\n", call, PSXM(n.a0),
                          PSXM(n.a1), n.ra);
            break;
        }
        case 0x22: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strcspn(\"%s\", \"%s\") from 0x%08x\n", call,
                          PSXM(n.a0), PSXM(n.a1), n.ra);
            break;
        }
        case 0x23: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strtok(\"%s\", \"%s\") from 0x%08x\n", call, PSXM(n.a0),
                          PSXM(n.a1), n.ra);
            break;
        }
        case 0x24: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strstr(\"%s\", \"%s\") from 0x%08x\n", call, PSXM(n.a0),
                          PSXM(n.a1), n.ra);
            break;
        }
        case 0x25: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:toupper('%c') from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x26: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:tolower('%c') from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x27: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:bcopy(0x%08x, 0x%08x, %i) from 0x%08x\n", call, n.a0,
                          n.a1, n.a2, n.ra);
            break;
        }
        case 0x28: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:bzero(0x%08x, %i) from 0x%08x\n", call, n.a0, n.a1,
                          n.ra);
            break;
        }
        case 0x29: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:bcmp(0x%08x, 0x%08x, %i) from 0x%08x\n", call, n.a0,
                          n.a1, n.a2, n.ra);
            break;
        }
        case 0x2a: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:memcpy(0x%08x, 0x%08x, %i) from 0x%08x\n", call, n.a0,
                          n.a1, n.a2, n.ra);
            break;
        }
        case 0x2b: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:memset(0x%08x, 0x%02x, %i) from 0x%08x\n", call, n.a0,
                          n.a1, n.a2, n.ra);
            break;
        }
        case 0x2c: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:memmove(0x%08x, 0x%08x, %i) from 0x%08x\n", call, n.a0,
                          n.a1, n.a2, n.ra);
            break;
        }
        case 0x2d: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:memcmp(0x%08x, 0x%08x, %i) from 0x%08x\n", call, n.a0,
                          n.a1, n.a2, n.ra);
            break;
        }
        case 0x2e: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:memchr(0x%08x, 0x%02x, %i) from 0x%08x\n", call, n.a0,
                          n.a1, n.a2, n.ra);
            break;
        }
        case 0x2f: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:rand() from 0x%08x\n", call, n.ra);
            break;
        }
        case 0x30: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:srand(%i) from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x31: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:qsort(0x%08x, %i, %i, 0x%08x) from 0x%08x\n", call,
                          n.a0, n.a1, n.a2, n.a3, n.ra);
            break;
        }
        case 0x32: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:strtod(\"%s\", 0x%08x) from 0x%08x\n", call, n.a0, n.a1,
                          n.ra);
            break;
        }
        case 0x33: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:malloc(%i) from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x34: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:free(0x%08x) from 0x%08x\n", call, n.a0, n.ra);
            break;
        }
        case 0x35: {
            uint32_t *cmp = (uint32_t *)PSXM(n.sp + 0x10);
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:lsearch(0x%08x, 0x%08x, %i, %i, 0x%08x) from 0x%08x\n",
                          call, n.a0, n.a1, n.a2, n.a3, *cmp, n.ra);
            break;
        }
        case 0x36: {
            uint32_t *cmp = (uint32_t *)PSXM(n.sp + 0x10);
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:bsearch(0x%08x, 0x%08x, %i, %i, 0x%08x) from 0x%08x\n",
                          call, n.a0, n.a1, n.a2, n.a3, *cmp, n.ra);
            break;
        }
        case 0x37: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:calloc(%i, %i) from 0x%08x\n", call, n.a0, n.a1, n.ra);
            break;
        }
        case 0x38: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:realloc(0x%08x, %i) from 0x%08x\n", call, n.a0, n.a1,
                          n.ra);
            break;
        }
        case 0x39: {
            g_system->log(LogClass::KERNEL, "KernelCall A0:%02X:initHeap(0x%08x, %i) from 0x%08x\n", call, n.a0, n.a1,
                          n.ra);
            break;
        }
        default: {
            g_system->log(LogClass::KERNEL, "KernelCall: unknown kernel call A0:%02X from 0x%08x\n", call, n.ra);
            break;
        }
    }
}

void PCSX::R3000Acpu::logB0KernelCall(uint32_t call) {
    auto &n = m_psxRegs.GPR.n;
    switch (call) {
        case 0x07: {
            g_system->log(LogClass::KERNEL, "KernelCall B0:%02X:deliverEvent(%s, %s) from 0x%08x\n", call,
                          Kernel::Events::Event::resolveClass(n.a0).c_str(),
                          Kernel::Events::Event::resolveSpec(n.a1).c_str(), n.ra);
            break;
        }
        case 0x08: {
            int id =
                Kernel::Events::getFirstFreeEvent(reinterpret_cast<const uint32_t *>(g_emulator->m_psxMem->g_psxM));
            g_system->log(LogClass::KERNEL, "KernelCall B0:%02X:openEvent(%s, %s, %s, 0x%08x) --> 0x%08x from 0x%08x\n",
                          call, Kernel::Events::Event::resolveClass(n.a0).c_str(),
                          Kernel::Events::Event::resolveSpec(n.a1).c_str(),
                          Kernel::Events::Event::resolveMode(n.a2).c_str(), n.a3, id | 0xf1000000, n.ra);
            break;
        }
        case 0x09: {
            Kernel::Events::Event ev{reinterpret_cast<const uint32_t *>(g_emulator->m_psxMem->g_psxM), n.a0};
            g_system->log(LogClass::KERNEL, "KernelCall B0:%02X:closeEvent(0x%08x {%s, %s}) from 0x%08x\n", call, n.a0,
                          ev.getClass().c_str(), ev.getSpec().c_str(), n.ra);
            break;
        }
        case 0x0a: {
            Kernel::Events::Event ev{reinterpret_cast<const uint32_t *>(g_emulator->m_psxMem->g_psxM), n.a0};
            g_system->log(LogClass::KERNEL, "KernelCall B0:%02X:waitEvent(0x%08x {%s, %s}) from 0x%08x\n", call, n.a0,
                          ev.getClass().c_str(), ev.getSpec().c_str(), n.ra);
            break;
        }
        case 0x0b: {
            Kernel::Events::Event ev{reinterpret_cast<const uint32_t *>(g_emulator->m_psxMem->g_psxM), n.a0};
            g_system->log(LogClass::KERNEL, "KernelCall B0:%02X:testEvent(0x%08x {%s, %s}) from 0x%08x\n", call, n.a0,
                          ev.getClass().c_str(), ev.getSpec().c_str(), n.ra);
            break;
        }
        case 0x0c: {
            Kernel::Events::Event ev{reinterpret_cast<const uint32_t *>(g_emulator->m_psxMem->g_psxM), n.a0};
            g_system->log(LogClass::KERNEL, "KernelCall B0:%02X:enableEvent(0x%08x {%s, %s}) from 0x%08x\n", call, n.a0,
                          ev.getClass().c_str(), ev.getSpec().c_str(), n.ra);
            break;
        }
        case 0x0d: {
            Kernel::Events::Event ev{reinterpret_cast<const uint32_t *>(g_emulator->m_psxMem->g_psxM), n.a0};
            g_system->log(LogClass::KERNEL, "KernelCall B0:%02X:disableEvent(0x%08x {%s, %s}) from 0x%08x\n", call,
                          n.a0, ev.getClass().c_str(), ev.getSpec().c_str(), n.ra);
            break;
        }
        case 0x20: {
            g_system->log(LogClass::KERNEL, "KernelCall B0:%02X:undeliverEvent(%s, %s) from 0x%08x\n", call,
                          Kernel::Events::Event::resolveClass(n.a0).c_str(),
                          Kernel::Events::Event::resolveSpec(n.a1).c_str(), n.ra);
            break;
        }
        default: {
            g_system->log(LogClass::KERNEL, "KernelCall: unknown kernel call B0:%02X from 0x%08x\n", call, n.ra);
            break;
        }
    }
}

void PCSX::R3000Acpu::logC0KernelCall(uint32_t call) {
    auto &n = m_psxRegs.GPR.n;
    switch (call) {
        default: {
            g_system->log(LogClass::KERNEL, "KernelCall: unknown kernel call C0:%02X from 0x%08x\n", call,
                          m_psxRegs.GPR.n.ra);
            break;
        }
    }
}

std::unique_ptr<PCSX::R3000Acpu> PCSX::Cpus::Interpreted() {
    std::unique_ptr<PCSX::R3000Acpu> cpu = getInterpreted();
    if (cpu->Implemented()) return cpu;
    return nullptr;
}

std::unique_ptr<PCSX::R3000Acpu> PCSX::Cpus::DynaRec() {
    std::unique_ptr<PCSX::R3000Acpu> cpu = getX86DynaRec();
    if (cpu->Implemented()) return cpu;
    return nullptr;
}
