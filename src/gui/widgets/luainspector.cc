/***************************************************************************
 *   Copyright (C) 2020 PCSX-Redux authors                                 *
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

#include "gui/widgets/luainspector.h"

#include "core/luawrapper.h"
#include "fmt/format.h"
#include "imgui.h"

void PCSX::Widgets::LuaInspector::dumpTree(const std::string& label, Lua* L, int i) {
    if (L->istable(i)) {
        if (!ImGui::TreeNode(label.c_str())) return;
        L->push();
        L->checkstack();
        while (L->next(i) != 0) {
            dumpTree(L->tostring(-2), L, L->gettop());
            L->pop();
        }
        ImGui::TreePop();
    } else {
        std::string typestring = fmt::format("({})", L->typestring(i));
        std::string entry = fmt::format("{:40} {:15} {}", label, typestring, L->tostring(i));
        ImGui::TextUnformatted(entry.c_str());
    }
}

void PCSX::Widgets::LuaInspector::draw(const char* title, Lua* L) {
    ImGui::SetNextWindowSize(ImVec2(520, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &m_show)) {
        ImGui::End();
        return;
    }

    static const char* displays[] = {"Globals", "Stack", "Registry"};

    if (ImGui::BeginCombo(_("Display"), displays[int(m_display)])) {
        for (int i = 0; i < (sizeof(displays) / sizeof(displays[0])); i++) {
            if (ImGui::Selectable(displays[i], int(m_display) == i)) {
                m_display = decltype(m_display)(i);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::BeginChild("tree");
    switch (m_display) {
        case Display::GLOBALS:
            dumpTree("_G", L, LUA_GLOBALSINDEX);
            break;
        case Display::REGISTRY:
            dumpTree("REGISTRY", L, LUA_REGISTRYINDEX);
            break;
        case Display::STACK: {
            int n = L->gettop();
            for (int i = 0; i < n; i++) dumpTree(fmt::format("{}", i), L, i);
            break;
        }
    }
    ImGui::EndChild();
    ImGui::End();
}
