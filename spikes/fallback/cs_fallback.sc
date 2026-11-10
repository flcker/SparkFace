// SPDX-License-Identifier: Apache-2.0
#include "bgfx_compute.sh"

IMAGE2D_RO(s_input, rgba8, 0);
IMAGE2D_WO(s_output, rgba8, 1);

NUM_THREADS(16, 16, 1)
void main()
{
    ivec2 coordinate = ivec2(gl_GlobalInvocationID.xy);
    vec4 value = imageLoad(s_input, coordinate);
    imageStore(s_output, coordinate, value);
}
