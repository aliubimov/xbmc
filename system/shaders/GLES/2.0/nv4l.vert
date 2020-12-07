varying vec2 interp_tc;
attribute vec4 in_pos;

void main() {
  interp_tc = in_pos.zw;
  gl_Position = vec4(in_pos.xy, 0, 1);
}

