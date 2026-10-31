// viewfix.h - keeps NieR's CPU-computed view-space data consistent with the eye camera (viewfix.cpp).
#pragma once
#include <d3d11.h>

void ViewFixBeforeDispatch(ID3D11DeviceContext* c, int kind);
void ViewFixAfterDispatch(ID3D11DeviceContext* c);
