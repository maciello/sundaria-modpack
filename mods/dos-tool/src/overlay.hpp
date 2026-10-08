#pragma once

namespace overlay {
    // Installs the D3D11 present/resize hooks via kiero. Returns true if the
    // render API was found and the hooks were bound.
    bool Init();
}
