#pragma once
#include <memory>

namespace Luna
{
class GameObject;

struct Selection
{
    int                        selectedIndex  = -1;
    std::shared_ptr<GameObject> selectedObject = nullptr;

    bool HasSelection() const { return selectedIndex >= 0 && selectedObject != nullptr; }
    void Clear() { selectedIndex = -1; selectedObject = nullptr; }
};

inline Selection& GetSelection()
{
    static Selection s_selection;
    return s_selection;
}

} // namespace Luna

