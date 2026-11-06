# bbtree++

A C++20 port of [bbtree](https://github.com/XiangpengHao/bbtree), the B+-tree
ordered map by Xiangpeng Hao. Work in progress: see [PLAN.md](PLAN.md).

- [PORTING.md](PORTING.md): where and why the port differs from the Rust original.
- [FUTURE_WORK.md](FUTURE_WORK.md): ideas left for later.
- [design/](design/): the original's design, which the port follows.

## Build

```bash
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
scripts/check.sh            # format, Clang and GCC builds, tests
scripts/check.sh --sanitize # plus AddressSanitizer + UBSan
```

## License

Licensed under either of Apache License, Version 2.0 or MIT license, at
your option; [LICENSE](LICENSE) holds both texts, as the original's does.
