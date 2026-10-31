# Build the injector

```text
py build.py <zig.exe> --azimuth [--test] --output <winmm.dll> --build-dir <cache>
```

Run the repository's top-level `build.py --zig <zig.exe> --test` for structure
validation, both host variants, samples, packaging and offline tests.
The host compiles twelve files. Extra Options is built separately in the
forza-plugins repository; no game profile changes host compilation.
