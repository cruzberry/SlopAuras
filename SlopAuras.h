#pragma once

#include <ToolboxUIPlugin.h>

class SlopAuras : public ToolboxUIPlugin {
public:
    const char* Name() const override { return "SlopAuras"; }

    void Draw(IDirect3DDevice9* pDevice) override;
    void DrawSettings() override;
    void LoadSettings(const wchar_t* folder) override;
    void SaveSettings(const wchar_t* folder) override;

private:
    std::vector<int> effect_ids;
};
