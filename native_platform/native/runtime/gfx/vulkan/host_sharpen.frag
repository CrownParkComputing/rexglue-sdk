#version 450
layout(set=0,binding=0) uniform sampler2D image;
layout(location=0) in vec2 texcoord;
layout(location=1) in vec4 tint;
layout(location=0) out vec4 result;
layout(push_constant) uniform Params { vec2 scale; vec2 offset; float sharpness; } p;
void main() {
  vec2 d=1.0/vec2(textureSize(image,0));
  vec3 c=texture(image,texcoord).rgb;
  vec3 a=texture(image,texcoord+vec2(d.x,0)).rgb;
  vec3 b=texture(image,texcoord-vec2(d.x,0)).rgb;
  vec3 e=texture(image,texcoord+vec2(0,d.y)).rgb;
  vec3 f=texture(image,texcoord-vec2(0,d.y)).rgb;
  vec3 lo=min(c,min(min(a,b),min(e,f)));
  vec3 hi=max(c,max(max(a,b),max(e,f)));
  result=vec4(clamp(c+p.sharpness*(4*c-a-b-e-f),lo,hi),1);
}
