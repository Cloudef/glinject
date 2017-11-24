# OpenGL injector thingy

For now can be used to filter and replace GLSL shaders in OpenGL programs.
In future, you may be able to do the same for models and textures as well.

## Compiling

```
gcc -std=c99 -fPIC -shared -Wl,-soname,glinject.so glinject.c -o glinject.so
```

Or optionally for 32bit:
```
gcc -m32 -std=c99 -fPIC -shared -Wl,-soname,glinject.so glinject.c -o glinject.so
```

## Usage

```
LD_PRELOAD="/path/to/glinject.so" ./program
```

If your program is 32bit, you'll need to compile a 32bit glinject instead.

## Env vars

* SHADER_FILTER: Filter by executing program with shader source as content of stdin.
                 The shader-filters dir in this repository contain some filters you can use.
