#version 150

uniform sampler2DRect map;

attribute vec2 position;
attribute vec2 uv;

varying vec2 vUv;

void main() {
  gl_Position = vec4(position, 0, 1);
  vUv = uv;
}
