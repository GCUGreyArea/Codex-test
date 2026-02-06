# JSONPath Library Build Notes

**Instructions To Create The System**

1. Create the project directories: `include/jsonpath`, `src`, `tests`.
2. Add a JSON data model and parser.
3. Add the JSONPath API header and implementation.
4. Add a GoogleTest `test_main` and JSONPath test suite.
5. Add a Makefile that builds a shared library and test binary.
6. Build the library and run tests with `make` and `make test`.

**Commands Run**

```sh
ls
cat AGENTS.md
ls -la
mkdir -p include/jsonpath src tests
cat <<'EOF' > include/jsonpath/json.hpp
...EOF
cat <<'EOF' > include/jsonpath/jsonpath.hpp
...EOF
cat <<'EOF' > src/json.cpp
...EOF
cat <<'EOF' > src/jsonpath.cpp
...EOF
cat <<'EOF' > tests/test_main.cpp
...EOF
cat <<'EOF' > tests/jsonpath_tests.cpp
...EOF
cat <<'EOF' > Makefile
...EOF
make
make
make test
ls -a
cat <<'EOF' > .gitignore
...EOF
cat .gitignore
cat <<'EOF' > gitignore.md
...EOF
```

**Final Summary**

- Implemented a C++ JSON parser and JSONPath compiler/evaluator, exposed via a small API and built as `build/libjsonpath.so`.
- Implemented a JSON representation optimized for search by storing arrays/objects as `std::shared_ptr<Json>` nodes with `unordered_map` lookup for object members.
- Added GoogleTest coverage for core JSONPath features (selectors, wildcards, unions, slices, recursive descent, filters, comparisons, and functions) and a `test_main`.
- Build and test via `make` and `make test`.
