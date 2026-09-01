// Shadow pass (instanced static geometry): position only, projected by the light.
// Uses the same VAO as the main pass, so instance matrix is at locations 3..6.
layout(location = 0) in vec3 aPos;
layout(location = 3) in mat4 iModel;
#include "common.glsl"
void main() {
    gl_Position = uLightVP * iModel * vec4(aPos, 1.0);
}
