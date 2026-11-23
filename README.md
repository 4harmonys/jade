# Jade

## Setup

Create a `llvm.ini` file in the root directory:
```ini
[binaries]
c = '/opt/homebrew/opt/llvm/bin/clang'
cpp = '/opt/homebrew/opt/llvm/bin/clang++'
llvm-config = '/opt/homebrew/opt/llvm/bin/llvm-config'
```

then run

```sh
meson setup build --native-file llvm.ini
```

and finally, to build

```sh
meson compile -C build
```

optionally run it!

```sh
./build/jade
```

but wait! it wont work, you need to set the env var: `ANTHROPIC_API_KEY_COLON_THREE`
