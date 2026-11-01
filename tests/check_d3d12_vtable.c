/* Compile-time check of the two COM vtable slots the D3D12 backend patches.
 * If this compiles, the slot numbers in layer/src/backend_d3d12.cpp are right for these headers.
 *
 *   x86_64-w64-mingw32-gcc -c tests/check_d3d12_vtable.c -o /dev/null \
 *       -Ilayer/third_party/DirectX-Headers/include
 */
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <windows.h>
#include <directx/d3d12.h>

_Static_assert(offsetof(ID3D12GraphicsCommandListVtbl, OMSetRenderTargets) / sizeof(void *) == 46,
               "OMSetRenderTargets is not vtable slot 46");
_Static_assert(offsetof(ID3D12DeviceVtbl, CreateRenderTargetView) / sizeof(void *) == 20,
               "CreateRenderTargetView is not vtable slot 20");
int main(void) { return 0; }
