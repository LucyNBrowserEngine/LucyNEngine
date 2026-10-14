# Third-Party Notices

The root [MIT license](LICENSE) covers LucyN-authored files unless a file has
its own notice. It does not replace the licenses for dependencies. Keep the
original notices when copying, modifying, or redistributing those components.

## Components in this repository

| Component | Location | License / notice |
|---|---|---|
| QuickJS-ng v0.17.0 | `third_party/quickjs/` | MIT; see its `LICENSE` and `VENDORED.txt`. |
| Mongoose 7.23 | `third_party/mongoose/` | GPL-2.0-only or a separate commercial license; see its `LICENSE` and `VENDORED.txt`. Its source files also carry notices for incorporated components. |
| stb headers | `third_party/stb/` | Each header offers a choice of MIT or public domain; see the license block at the end of each file and `VENDORED.txt`. |
| PT Sans Regular | `third_party/fonts/` | SIL Open Font License 1.1; see `OFL.txt` and `VENDORED.txt`. |

## External build dependencies

Gumbo, libcurl (or curl-impersonate), and optional SDL2 are obtained from the
system or separately supplied bundles; they are not covered by the root MIT
license. Their applicable notices are provided by their respective upstream
projects or package distributions. When distributing a curl-impersonate
bundle, preserve the `LICENSE_*` files included with that bundle, as they cover
its bundled dependencies. See [third_party/README.md](third_party/README.md).

## Important Mongoose distribution note

The default full application build links `modules/server`, which uses the
vendored Mongoose source. Mongoose's included terms offer GPL-2.0-only or a
separate commercial license. The LucyN MIT license does not relicense Mongoose
or remove its conditions from a combined binary. Review the included Mongoose
terms and upstream [licensing information](https://mongoose.ws/licensing/)
before distributing an application that links it. The standalone MIT license
for LucyN-authored code does not by itself make the default combined app an
MIT-only distribution.
