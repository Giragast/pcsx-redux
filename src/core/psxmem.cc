/***************************************************************************
 *   Copyright (C) 2023 PCSX-Redux authors                                 *
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

#include "core/psxmem.h"

#include <zlib.h>

#include <map>
#include <string_view>

#include "core/pio-cart.h"
#include "core/psxhw.h"
#include "core/r3000a.h"
#include "mips/common/util/encoder.hh"
#include "support/file.h"
#include "supportpsx/binloader.h"

static const std::map<uint32_t, std::string_view> s_knownBioses = {
#ifdef USE_ADLER
    {0x1002e6b5, "SCPH-1002 (EU)"},
    {0x1ac46cf1, "SCPH-5000 (JP)"},
    {0x24e21a0e, "SCPH-7003 (US)"},
    {0x38f5c1fe, "SCPH-1000 (JP)"},
    {0x42ea6879, "SCPH-1002 - DTLH-3002 (EU)"},
    {0x48ba1524, "????"},
    {0x4e501b56, "SCPH-5502 - SCPH-5552 (2) (EU)"},
    {0x560e2da1, "????"},
    {0x61e5b760, "SCPH-7001 (US)"},
    {0x649db764, "SCPH-7502 (EU)"},
    {0x68d2dd36, "????"},
    {0x68ee15cc, "SCPH-5502 - SCPH-5552 (EU)"},
    {0x7de956a4, "SCPH-101"},
    {0x80a156a8, "????"},
    {0x9e7d4faa, "SCPH-3000 (JP)"},
    {0x9eff111b, "SCPH-7000 (JP)"},
    {0xa6cf18fe, "SCPH-5500 (JP)"},
    {0xa8e56981, "SCPH-3500 (JP)"},
    {0xb6ef0d64, "????"},
    {0xd10b6509, "????"},
    {0xdaa2e0a6, "SCPH-1001 - DTLH-3000 (US)"},
    {0xe7ca4fad, "????"},
    {0xf380c9ff, "SCPH-5000"},
    {0xfb4afc11, "SCPH-5000 (2)"},
#else
    {0x0bad7ea9, "????"},
    {0x171bdcec, "SCPH-101"},
    {0x1e26792f, "SCPH-1002 - DTLH-3002 (EU)"},
    {0x24fc7e17, "SCPH-5000 (JP)"},
    {0x318178bf, "SCPH-7502 (EU)"},
    {0x3539def6, "SCPH-3000 (JP)"},
    {0x37157331, "SCPH-1001 - DTLH-3000 (US)"},
    {0x3b601fc8, "SCPH-1000 (JP)"},
    {0x4d9e7c86, "SCPH-5502 - SCPH-5552 (2) (EU)"},
    {0x502224b6, "SCPH-7001 (US)"},
    {0x55847d8c, "????"},
    {0x76b880e5, "????"},
    {0x826ac185, "SCPH-5000"},
    {0x86c30531, "????"},
    {0x8c93a399, "SCPH-5000 (2)"},
    {0x8d8cb7e4, "SCPH-7003 (US)"},
    {0x9bb87c4b, "SCPH-1002 (EU)"},
    {0xaff00f2f, "????"},
    {0xbc190209, "SCPH-3500 (JP)"},
    {0xd786f0b9, "SCPH-5502 - SCPH-5552 (EU)"},
    {0xdecb22f5, "????"},
    {0xec541cd0, "SCPH-7000 (JP)"},
    {0xf2af798b, "????"},
    {0xff3eeb8c, "SCPH-5500 (JP)"},
#endif
};

int PCSX::Memory::init() {
    m_readLUT = (uint8_t **)calloc(0x10000, sizeof(void *));
    m_writeLUT = (uint8_t **)calloc(0x10000, sizeof(void *));

    uint32_t pid = 0;
#if defined(_WIN32) || defined(_WIN64)
    pid = static_cast<uint32_t>(GetCurrentProcessId());
#elif defined(__linux__) || (defined(__APPLE__) && defined(__MACH__))
    pid = static_cast<uint32_t>(getpid());
#endif

    // Init all memory as named mappings
    m_wram.init(fmt::format("pcsx-redux-wram-{}", pid).c_str(), 0x00800000);

    m_exp1 = (uint8_t *)calloc(0x00800000, 1);
    m_hard = (uint8_t *)calloc(0x00010000, 1);
    m_bios = (uint8_t *)calloc(0x00080000, 1);

    if (m_readLUT == NULL || m_writeLUT == NULL || m_wram.m_mem == NULL || m_exp1 == NULL || m_bios == NULL ||
        m_hard == NULL) {
        g_system->message("%s", _("Error allocating memory!"));
        return -1;
    }

    // EXP1
    if (g_emulator->settings.get<Emulator::SettingPIOConnected>().value) {
        // Don't overwrite LUTs if not connected, in case these have been set externally
        g_emulator->m_pioCart->setLuts();
    }

    for (int i = 0; i < 0x08; i++) {
        m_readLUT[i + 0x1fc0] = (uint8_t *)&m_bios[i << 16];
    }

    memcpy(m_readLUT + 0x9fc0, m_readLUT + 0x1fc0, 0x08 * sizeof(void *));
    memcpy(m_readLUT + 0xbfc0, m_readLUT + 0x1fc0, 0x08 * sizeof(void *));

    setLuts();

    m_memoryAsFile = new MemoryAsFile(this);

    return 0;
}

bool PCSX::Memory::loadEXP1FromFile(std::filesystem::path rom_path) {
    const size_t exp1_size = 0x00040000;
    bool result = false;

    auto &exp1Path = rom_path;
    if (!exp1Path.empty()) {
        IO<File> f(new PosixFile(exp1Path.string()));
        if (f->failed()) {
            g_system->printf(_("Could not open EXP1:\"%s\".\n"), exp1Path.string());
        }

        if (!f->failed()) {
            size_t rom_size = (f->size() > exp1_size) ? exp1_size : f->size();
            memset(m_exp1, 0xff, exp1_size);
            f->read(m_exp1, rom_size);
            f->close();
            PCSX::g_system->printf(_("Loaded %i bytes to EXP1 from file: %s\n"), rom_size, exp1Path.string());
            result = true;
        }
    } else {
        // Empty path passed to function, wipe memory and treat as success
        memset(m_exp1, 0xff, exp1_size);
        result = true;
    }

    if (result) {
        g_emulator->settings.get<Emulator::SettingEXP1Filepath>().value = rom_path;
    }

    return result;
}

void PCSX::Memory::reset() {
    const uint32_t bios_size = 0x00080000;
    const uint32_t exp1_size = 0x00040000;
    memset(m_wram.m_mem, 0, 0x00800000);
    memset(m_exp1, 0xff, exp1_size);
    memset(m_bios, 0, bios_size);
    static const uint32_t nobios[6] = {
        Mips::Encoder::lui(Mips::Encoder::Reg::V0, 0xbfc0),  // v0 = 0xbfc00000
        Mips::Encoder::lui(Mips::Encoder::Reg::V1, 0x1f80),  // v1 = 0x1f800000
        Mips::Encoder::addiu(Mips::Encoder::Reg::T0, Mips::Encoder::Reg::V0, sizeof(nobios)),
        Mips::Encoder::sw(Mips::Encoder::Reg::T0, 0x2084, Mips::Encoder::Reg::V1),  // display notification
        Mips::Encoder::j(0xbfc00000),
        Mips::Encoder::sb(Mips::Encoder::Reg::R0, 0x2081, Mips::Encoder::Reg::V1),  // pause
    };

    int index = 0;
    for (auto w : nobios) {
        m_bios[index++] = w & 0xff;
        w >>= 8;
        m_bios[index++] = w & 0xff;
        w >>= 8;
        m_bios[index++] = w & 0xff;
        w >>= 8;
        m_bios[index++] = w & 0xff;
        w >>= 8;
    }
    strcpy((char *)m_bios + index, _(R"(
                   No BIOS loaded, emulation halted.

Set a BIOS file into the configuration, and do a hard reset of the emulator.
The distributed OpenBIOS.bin file can be an appropriate BIOS replacement.
)"));

    uint32_t nobioscrc = crc32(0L, Z_NULL, 0);
    nobioscrc = crc32(nobioscrc, m_bios, bios_size);

    // Load BIOS
    {
        auto &biosPath = g_emulator->settings.get<Emulator::SettingBios>().value;
        IO<File> f(new PosixFile(biosPath.string()));
        if (f->failed()) {
            g_system->printf(_("Could not open BIOS:\"%s\". Retrying with the OpenBIOS\n"), biosPath.string());

            g_system->findResource(
                [&f](const std::filesystem::path &filename) {
                    f.setFile(new PosixFile(filename));
                    return !f->failed();
                },
                "openbios.bin", "resources", std::filesystem::path("src") / "mips" / "openbios");
            if (f->failed()) {
                g_system->printf(
                    _("Could not open OpenBIOS fallback. Things won't work properly.\nAdd a valid BIOS in the "
                      "configuration "
                      "and hard reset.\n"));
            } else {
                biosPath = f->filename();
            }
        }

        if (!f->failed()) {
            BinaryLoader::Info i;
            if (!BinaryLoader::load(f, getMemoryAsFile(), i, g_emulator->m_cpu->m_symbols)) {
                f->rSeek(0);
                f->read(m_bios, bios_size);
            }
            f->close();
            PCSX::g_system->printf(_("Loaded BIOS: %s\n"), biosPath.string());
        }
    }

    if (!g_emulator->settings.get<Emulator::SettingEXP1Filepath>().value.empty()) {
        loadEXP1FromFile(g_emulator->settings.get<Emulator::SettingEXP1Filepath>().value);
    }

    uint32_t crc = crc32(0L, Z_NULL, 0);
    m_biosCRC = crc = crc32(crc, m_bios, bios_size);
    auto it = s_knownBioses.find(crc);
    if (it != s_knownBioses.end()) {
        g_system->printf(_("Known BIOS detected: %s (%08x)\n"), it->second, crc);
    } else if (strncmp((const char *)&m_bios[0x78], "OpenBIOS", 8) == 0) {
        g_system->printf(_("OpenBIOS detected (%08x)\n"), crc);
    } else if (crc != nobioscrc) {
        g_system->printf(_("Unknown bios loaded (%08x)\n"), crc);
    }
}

void PCSX::Memory::shutdown() {
    free(m_exp1);
    free(m_hard);
    free(m_bios);

    free(m_readLUT);
    free(m_writeLUT);
}

uint8_t PCSX::Memory::read8(uint32_t address) {
    g_emulator->m_cpu->m_regs.cycle += 1;
    const uint32_t page = address >> 16;
    const auto pointer = (uint8_t *)m_readLUT[page];
    const bool pioConnected = g_emulator->settings.get<Emulator::SettingPIOConnected>().value;

    if (pointer != nullptr) {
        const uint32_t offset = address & 0xffff;
        return *(pointer + offset);
    } else {
        if (page == 0x1f80 || page == 0x9f80 || page == 0xbf80) {
            if ((address & 0xffff) < 0x400) {
                return m_hard[address & 0x3ff];
            } else {
                return g_emulator->m_hw->read8(address);
            }
        } else if ((page & 0x1fff) >= 0x1f00 && (page & 0x1fff) < 0x1f80 && pioConnected) {
            return g_emulator->m_pioCart->read8(address);
        } else if (sendReadToLua(address, 1)) {
            auto L = *g_emulator->m_lua;
            const uint8_t ret = L.tonumber();
            L.pop();
            g_system->log(LogClass::HARDWARE, _("8-bit read redirected to Lua for address: %8.8lx\n"), address);
            return ret;
        } else if (address == 0x1f000004 || address == 0x1f000084) {
            // EXP1 not mapped, likely the bios looking for pre/post boot entry point
            // We probably don't want to pause here so just throw it a dummy value
            return 0xff;
        } else {
            g_system->log(LogClass::CPU, _("8-bit read from unknown address: %8.8lx\n"), address);
            if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::Debug>()) {
                g_system->pause();
            }
            return 0xff;
        }
    }
}

uint16_t PCSX::Memory::read16(uint32_t address) {
    g_emulator->m_cpu->m_regs.cycle += 1;
    const uint32_t page = address >> 16;
    const auto pointer = (uint8_t *)m_readLUT[page];
    const bool pioConnected = g_emulator->settings.get<Emulator::SettingPIOConnected>().value;

    if (pointer != nullptr) {
        const uint32_t offset = address & 0xffff;
        return SWAP_LEu16(*(uint16_t *)(pointer + offset));
    } else {
        if (page == 0x1f80 || page == 0x9f80 || page == 0xbf80) {
            if ((address & 0xffff) < 0x400) {
                uint16_t *ptr = (uint16_t *)&m_hard[address & 0x3ff];
                return SWAP_LEu16(*ptr);
            } else {
                return g_emulator->m_hw->read16(address);
            }
        } else if ((page & 0x1fff) >= 0x1f00 && (page & 0x1fff) < 0x1f80 && pioConnected) {
            return g_emulator->m_pioCart->read8(address);
        } else if (sendReadToLua(address, 2)) {
            auto L = *g_emulator->m_lua;
            const uint16_t ret = L.tonumber();
            L.pop();
            g_system->log(LogClass::HARDWARE, _("16-bit read redirected to Lua for address: %8.8lx\n"), address);
            return ret;
        } else {
            g_system->log(LogClass::CPU, _("16-bit read from unknown address: %8.8lx\n"), address);
            if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::Debug>()) {
                g_system->pause();
            }
            return 0xffff;
        }
    }
}

uint32_t PCSX::Memory::read32(uint32_t address, ReadType readType) {
    if (readType == ReadType::Data) g_emulator->m_cpu->m_regs.cycle += 1;
    const uint32_t page = address >> 16;
    const auto pointer = (uint8_t *)m_readLUT[page];
    const bool pioConnected = g_emulator->settings.get<Emulator::SettingPIOConnected>().value;

    if (pointer != nullptr) {
        const uint32_t offset = address & 0xffff;
        return SWAP_LEu32(*(uint32_t *)(pointer + offset));
    } else {
        if (page == 0x1f80 || page == 0x9f80 || page == 0xbf80) {
            if ((address & 0xffff) < 0x400) {
                uint32_t *ptr = (uint32_t *)&m_hard[address & 0x3ff];
                return SWAP_LEu32(*ptr);
            } else {
                return g_emulator->m_hw->read32(address);
            }
        } else if ((page & 0x1fff) >= 0x1f00 && (page & 0x1fff) < 0x1f80 && pioConnected) {
            return g_emulator->m_pioCart->read32(address);
        } else if (sendReadToLua(address, 4)) {
            auto L = *g_emulator->m_lua;
            const uint32_t ret = L.tonumber();
            L.pop();
            g_system->log(LogClass::HARDWARE, _("32-bit read redirected to Lua for address: %8.8lx\n"), address);
            return ret;
        } else {
            if (m_writeok) {
                g_system->log(LogClass::CPU, _("32-bit read from unknown address: %8.8lx\n"), address);
                if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::Debug>()) {
                    g_system->pause();
                }
            }
            return 0xffffffff;
        }
    }
}

int PCSX::Memory::sendReadToLua(const uint32_t address, const size_t size) {
    // Grab a local pointer for our Lua VM interpreter
    auto L = *g_emulator->m_lua;
    int nresult = 0;
    // Try getting the symbol 'UnknownMemoryRead' from the global space, and put it on top of the stack
    L.getfield("UnknownMemoryRead", LUA_GLOBALSINDEX);
    // If top-of-stack is not nil, then we're going to try calling it.
    if (!L.isnil()) {
        try {
            // Call the function. Technically we should empty
            // the stack afterward to clean potential return
            // values from the Lua code, this is a bug in the gui code.
            const int top = L.gettop();
            L.push(lua_Number(address));
            L.push(lua_Number(size));
            nresult = L.pcall(2);
            // Discard anything more than 1 result
            for (int n = 1; n < nresult; n++) {
                L.pop();
            }
        } catch (...) {
            // If there is any error while executing the Lua code,
            // push the string "UnknownMemoryRead" on top of the stack,
            L.push("UnknownMemoryRead");
            // then push the value "nil" (rough equivalent of NULL in Lua)
            L.push();
            // This will pop a key/value pair from the stack, and set
            // the corresponding value in the global space. This effectively
            // deletes the global function on errors.
            L.settable(LUA_GLOBALSINDEX);
        }
    } else {
        // This pops the nil from the stack.
        L.pop();
    }

    return nresult;
}

bool PCSX::Memory::sendWriteToLua(const uint32_t address, const size_t size, uint32_t value) {
    auto L = *g_emulator->m_lua;
    bool write_handled = false;

    L.getfield("UnknownMemoryWrite", LUA_GLOBALSINDEX);
    if (!L.isnil()) {
        try {
            const int top = L.gettop();
            L.push(lua_Number(address));
            L.push(lua_Number(size));
            L.push(lua_Number(value));
            int nresult = L.pcall(3);

            if (nresult > 0) {
                // Discard anything more than 1 result
                for (int n = 1; n < nresult; n++) {
                    L.pop();
                }

                write_handled = L.toboolean();
                L.pop();
            }

        } catch (...) {
            L.push("UnknownMemoryWrite");
            L.push();
            L.settable(LUA_GLOBALSINDEX);
        }
    } else {
        L.pop();
    }

    return write_handled;
}

void PCSX::Memory::write8(uint32_t address, uint32_t value) {
    g_emulator->m_cpu->m_regs.cycle += 1;
    const uint32_t page = address >> 16;
    const auto pointer = (uint8_t *)m_writeLUT[page];
    const bool pioConnected = g_emulator->settings.get<Emulator::SettingPIOConnected>().value;

    if (pointer != nullptr) {
        const uint32_t offset = address & 0xffff;
        *(pointer + offset) = static_cast<uint8_t>(value);
        g_emulator->m_cpu->Clear((address & (~3)), 1);
    } else {
        if (page == 0x1f80 || page == 0x9f80 || page == 0xbf80) {
            if ((address & 0xffff) < 0x400) {
                m_hard[address & 0x3ff] = value;
            } else {
                g_emulator->m_hw->write8(address, value);
            }
        } else if ((page & 0x1fff) >= 0x1f00 && (page & 0x1fff) < 0x1f80 && pioConnected) {
            g_emulator->m_pioCart->write8(address, value);
        } else if (sendWriteToLua(address, 1, value)) {
            g_system->log(LogClass::HARDWARE, _("8-bit write redirected to Lua for address: %8.8lx\n"), address);
        } else {
            g_system->log(LogClass::CPU, _("8-bit write to unknown address: %8.8lx\n"), address);
            if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::Debug>()) {
                g_system->pause();
            }
        }
    }
}

void PCSX::Memory::write16(uint32_t address, uint32_t value) {
    g_emulator->m_cpu->m_regs.cycle += 1;
    const uint32_t page = address >> 16;
    const auto pointer = (uint8_t *)m_writeLUT[page];
    const bool pioConnected = g_emulator->settings.get<Emulator::SettingPIOConnected>().value;

    if (pointer != nullptr) {
        const uint32_t offset = address & 0xffff;
        *(uint16_t *)(pointer + offset) = SWAP_LEu16(static_cast<uint16_t>(value));
        g_emulator->m_cpu->Clear((address & (~3)), 1);
    } else {
        if (page == 0x1f80 || page == 0x9f80 || page == 0xbf80) {
            if ((address & 0xffff) < 0x400) {
                uint16_t *ptr = (uint16_t *)&m_hard[address & 0x3ff];
                *ptr = SWAP_LEu16(value);
            } else {
                g_emulator->m_hw->write16(address, value);
            }
        } else if ((page & 0x1fff) >= 0x1f00 && (page & 0x1fff) < 0x1f80 && pioConnected) {
            g_emulator->m_pioCart->write16(address, value);
        } else if (sendWriteToLua(address, 2, value)) {
            g_system->log(LogClass::HARDWARE, _("16-bit write redirected to Lua for address: %8.8lx\n"), address);
        } else {
            g_system->log(LogClass::CPU, _("16-bit write to unknown address: %8.8lx\n"), address);
            if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::Debug>()) {
                g_system->pause();
            }
        }
    }
}

void PCSX::Memory::write32(uint32_t address, uint32_t value) {
    g_emulator->m_cpu->m_regs.cycle += 1;
    const uint32_t page = address >> 16;
    const auto pointer = (uint8_t *)m_writeLUT[page];
    const bool pioConnected = g_emulator->settings.get<Emulator::SettingPIOConnected>().value;

    if (pointer != nullptr) {
        const uint32_t offset = address & 0xffff;
        *(uint32_t *)(pointer + offset) = SWAP_LEu32(value);
        g_emulator->m_cpu->Clear((address & (~3)), 1);
    } else {
        if (page == 0x1f80 || page == 0x9f80 || page == 0xbf80) {
            if ((address & 0xffff) < 0x400) {
                uint32_t *ptr = (uint32_t *)&m_hard[address & 0x3ff];
                *ptr = SWAP_LEu32(value);
            } else {
                g_emulator->m_hw->write32(address, value);
            }
        } else if ((page & 0x1fff) >= 0x1f00 && (page & 0x1fff) < 0x1f80 && pioConnected) {
            g_emulator->m_pioCart->write32(address, value);
        } else if (address != 0xfffe0130) {
            if (!m_writeok) g_emulator->m_cpu->Clear(address, 1);

            if (m_writeok) {
                g_system->log(LogClass::CPU, _("32-bit write to unknown address: %8.8lx\n"), address);
                if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::Debug>()) {
                    g_system->pause();
                }
            }
        } else {
            // a0-44: used for cache flushing
            switch (value) {
                case 0x800:
                case 0x804:
                    if (m_writeok == 0) break;
                    m_writeok = 0;
                    setLuts();

                    g_emulator->m_cpu->invalidateCache();
                    break;
                case 0x00:
                case 0x1e988:
                    if (m_writeok == 1) break;
                    m_writeok = 1;
                    setLuts();
                    break;
                default:
                    if (sendWriteToLua(address, 4, value)) {
                        g_system->log(LogClass::HARDWARE, _("32-bit write redirected to Lua for address: %8.8lx\n"),
                                      address);
                    } else {
                        g_system->log(LogClass::CPU, _("32-bit write to unknown address: %8.8lx\n"), address);
                        if (g_emulator->settings.get<Emulator::SettingDebugSettings>().get<Emulator::DebugSettings::Debug>()) {
                            g_system->pause();
                        }
                    }
                    break;
            }
        }
    }
}

const void *PCSX::Memory::pointerRead(uint32_t address) {
    const auto page = address >> 16;

    if (page == 0x1f80 || page == 0x9f80 || page == 0xbf80) {
        if ((address & 0xffff) < 0x400)
            return &m_hard[address & 0x3ff];
        else {
            switch (address) {  // IO regs that are safe to read from directly
                case 0x1f801080:
                case 0x1f801084:
                case 0x1f801088:
                case 0x1f801090:
                case 0x1f801094:
                case 0x1f801098:
                case 0x1f8010a0:
                case 0x1f8010a4:
                case 0x1f8010a8:
                case 0x1f8010b0:
                case 0x1f8010b4:
                case 0x1f8010b8:
                case 0x1f8010c0:
                case 0x1f8010c4:
                case 0x1f8010c8:
                case 0x1f8010d0:
                case 0x1f8010d4:
                case 0x1f8010d8:
                case 0x1f8010e0:
                case 0x1f8010e4:
                case 0x1f8010e8:
                case 0x1f801070:
                case 0x1f801074:
                case 0x1f8010f0:
                case 0x1f8010f4:
                    return &m_hard[address & 0xffff];

                default:
                    return nullptr;
            }
        }
    } else {
        const auto pointer = (char *)(m_readLUT[page]);
        if (pointer != nullptr) {
            return (void *)(pointer + (address & 0xffff));
        }
        return nullptr;
    }
}

const void *PCSX::Memory::pointerWrite(uint32_t address, int size) {
    const auto page = address >> 16;

    if (page == 0x1f80 || page == 0x9f80 || page == 0xbf80) {
        if ((address & 0xffff) < 0x400)
            return &m_hard[address & 0x3ff];
        else {
            switch (address) {
                // IO regs that are safe to write to directly. For some of these,
                // Writing a 8-bit/16-bit value actually writes the entire 32-bit reg, so they're not safe to write
                // directly
                case 0x1f801080:
                case 0x1f801084:
                case 0x1f801090:
                case 0x1f801094:
                case 0x1f8010a0:
                case 0x1f8010a4:
                case 0x1f8010b0:
                case 0x1f8010b4:
                case 0x1f8010c0:
                case 0x1f8010c4:
                case 0x1f8010d0:
                case 0x1f8010d4:
                case 0x1f8010e0:
                case 0x1f8010e4:
                case 0x1f801074:
                case 0x1f8010f0:
                    return size == 32 ? &m_hard[address & 0xffff] : nullptr;

                default:
                    return nullptr;
            }
        }
    } else {
        const auto pointer = (char *)(m_writeLUT[page]);
        if (pointer != nullptr) {
            return (void *)(pointer + (address & 0xffff));
        }
        return nullptr;
    }
}

void PCSX::Memory::setLuts() {
    int max = (m_hard[0x1061] & 0x1) ? 0x80 : 0x20;
    if (!g_emulator->settings.get<Emulator::Setting8MB>()) max = 0x20;
    for (int i = 0; i < 0x80; i++) m_readLUT[i + 0x0000] = (uint8_t *)&m_wram.m_mem[(i & (max - 1)) << 16];
    memcpy(m_readLUT + 0x8000, m_readLUT, 0x80 * sizeof(void *));
    memcpy(m_readLUT + 0xa000, m_readLUT, 0x80 * sizeof(void *));
    if (m_writeok) {
        memcpy(m_writeLUT + 0x0000, m_readLUT, 0x80 * sizeof(void *));
        memcpy(m_writeLUT + 0x8000, m_readLUT, 0x80 * sizeof(void *));
        memcpy(m_writeLUT + 0xa000, m_readLUT, 0x80 * sizeof(void *));
    } else {
        memset(m_writeLUT + 0x0000, 0, 0x80 * sizeof(void *));
        memset(m_writeLUT + 0x8000, 0, 0x80 * sizeof(void *));
        memset(m_writeLUT + 0xa000, 0, 0x80 * sizeof(void *));
    }
    g_system->m_eventBus->signal(PCSX::Events::Memory::SetLuts{});
}

std::string_view PCSX::Memory::getBiosVersionString() {
    auto it = s_knownBioses.find(m_biosCRC);
    if (it == s_knownBioses.end()) return "Unknown";
    return it->second;
}

ssize_t PCSX::Memory::MemoryAsFile::rSeek(ssize_t pos, int wheel) {
    switch (wheel) {
        case SEEK_SET:
            m_ptrR = pos;
            break;
        case SEEK_END:
            m_ptrR = c_size - pos;
            break;
        case SEEK_CUR:
            m_ptrR += pos;
            break;
    }
    m_ptrR = std::max(std::min(m_ptrR, c_size), size_t(0));
    return m_ptrR;
}

ssize_t PCSX::Memory::MemoryAsFile::wSeek(ssize_t pos, int wheel) {
    switch (wheel) {
        case SEEK_SET:
            m_ptrW = pos;
            break;
        case SEEK_END:
            m_ptrW = c_size - pos;
            break;
        case SEEK_CUR:
            m_ptrW += pos;
            break;
    }
    m_ptrW = std::max(std::min(m_ptrW, c_size), size_t(0));
    return m_ptrW;
}

ssize_t PCSX::Memory::MemoryAsFile::readAt(void *dest, size_t size, size_t ptr) {
    if (ptr >= c_size) return 0;
    size_t ret = size = cappedSize(size, ptr);
    while (size) {
        auto blockSize = std::min(size, c_blockSize - (ptr % c_blockSize));
        readBlock(dest, blockSize, ptr);
        size -= blockSize;
        ptr += blockSize;
        dest = (char *)dest + blockSize;
    }
    return ret;
}

ssize_t PCSX::Memory::MemoryAsFile::writeAt(const void *src, size_t size, size_t ptr) {
    if (ptr >= c_size) return 0;
    size_t ret = size = cappedSize(size, ptr);
    while (size) {
        auto blockSize = std::min(size, c_blockSize - (ptr % c_blockSize));
        writeBlock(src, blockSize, ptr);
        size -= blockSize;
        ptr += blockSize;
        src = (char *)src + blockSize;
    }
    return ret;
}

void PCSX::Memory::MemoryAsFile::readBlock(void *dest, size_t size, size_t ptr) {
    auto block = m_memory->m_readLUT[ptr / c_blockSize];
    if (!block) {
        memset(dest, 0, size);
        return;
    }
    auto offset = ptr % c_blockSize;
    auto toCopy = std::min(size, c_blockSize - offset);
    memcpy(dest, block + offset, toCopy);
}

void PCSX::Memory::MemoryAsFile::writeBlock(const void *src, size_t size, size_t ptr) {
    // Yes. That's not a bug nor a typo.
    auto block = m_memory->m_readLUT[ptr / c_blockSize];
    if (!block) {
        return;
    }
    auto offset = ptr % c_blockSize;
    auto toCopy = std::min(size, c_blockSize - offset);
    memcpy(block + offset, src, toCopy);
}
