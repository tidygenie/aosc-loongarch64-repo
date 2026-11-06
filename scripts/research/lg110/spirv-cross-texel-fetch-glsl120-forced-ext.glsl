#version 120
#extension GL_EXT_gpu_shader4 : require

uniform sampler2D tex;

void main()
{
    vec3 gradient = vec3(0.0, gl_FragCoord.y / 480.0, 0.0);
    vec3 fetched = texelFetch2D(tex, ivec2(0), 0).xyz;
    gl_FragData[0] = vec4(gradient + (fetched * 9.9999999747524270787835121154785e-07), 1.0);
}

