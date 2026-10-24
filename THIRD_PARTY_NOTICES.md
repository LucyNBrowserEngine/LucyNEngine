# Third-Party Notices

The root [MIT license](LICENSE) covers LucyN-authored files unless a file has
its own notice. It does not replace the licenses for dependencies. Keep the
original notices when copying, modifying, or redistributing those components.

## Components in this repository

| Component | Location | License / notice |
|---|---|---|
| Mongoose 7.23 | `third_party/mongoose/` | GPL-2.0-only or a separate commercial license; see its `LICENSE` and `VENDORED.txt`. Its source files also carry notices for incorporated components. |

## External build dependencies

The engine is driven by the **Chromium Embedded Framework** (CEF), whose
binary distribution is downloaded into `third_party/cef/` by
`scripts/install.sh` and is not committed to this repository. Chromium and CEF are BSD-3-Clause licensed;
preserve the `LICENSE.txt` shipped inside the distribution when redistributing
it, and note that the Chromium inside it carries many third-party notices of
its own. See [third_party/README.md](third_party/README.md).

## Important Mongoose distribution note

The default full application build links `modules/server`, which uses the
vendored Mongoose source. Mongoose's included terms offer GPL-2.0-only or a
separate commercial license. The LucyN MIT license does not relicense Mongoose
or remove its conditions from a combined binary. Review the included Mongoose
terms and upstream [licensing information](https://mongoose.ws/licensing/)
before distributing an application that links it. The standalone MIT license
for LucyN-authored code does not by itself make the default combined app an
MIT-only distribution.
