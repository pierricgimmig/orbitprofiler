//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------

#include "UserPlugin.h"
#include "Platform.h"
#include <iostream>

//-----------------------------------------------------------------------------
void UserPlugin::Draw(ImGuiContext* a_ImguiContext, int a_Width, int a_Height) {
  ImGui::SetCurrentContext(a_ImguiContext);

  if (ImGui::Button("Plugin Test Button!!")) {
    std::cout << "Plugin button!\n";
  }
}

//-----------------------------------------------------------------------------
void UserPlugin::ReceiveUserData(const Orbit::UserData* a_Data) {}

//-----------------------------------------------------------------------------
void UserPlugin::ReceiveOrbitData(const Orbit::Data* a_Data) {}