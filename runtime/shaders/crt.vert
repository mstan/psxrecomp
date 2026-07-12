#version 450
/* Fullscreen triangle for the CRT present pass. The viewport is set to the
 * letterbox rect, so v_uv [0,1] spans exactly the displayed picture. */
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
