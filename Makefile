#############################################
# A thin wrapper around cmake and the python build tools
#
# Don't use this for production builds - just
# eliminates keystrokes while developing

all: debug

SEED := $(shell od -An -N4 -tu4 /dev/urandom 2>/dev/null | tr -dc 0-9)
SEED := $(or $(SEED),123142)

COMMIT := $(shell git rev-parse HEAD 2>/dev/null || echo unknown)

DEBUG_BIN := build/debug/bin

############ Debug
.PHONY: debug
debug: build/debug/CMakeCache.txt
	cmake --build build/debug -j12

build/debug/CMakeCache.txt:
	cmake -S . -B build/debug \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON

############ Testing
.PHONY: test
test: test-unit test-simtest

.PHONY: test-unit
test-unit: debug
	exe=$(DEBUG_BIN)/unit_tests; [ -f "$$exe.exe" ] && exe="$$exe.exe"; \
	"$$exe" $(SEED)

.PHONY: test-simtest
test-simtest: debug
	exe=$(DEBUG_BIN)/ns_simtest; [ -f "$$exe.exe" ] && exe="$$exe.exe"; \
	"$$exe" \
		--dbname simtest.db \
		--duration 50 \
		--seed $(SEED) \
		--commit-hash $(COMMIT) \
		--seqid $(SEED)

############ Release
.PHONY: release-package
release-package: release
	rm -rf build/release/package
	cmake --install build/release --prefix build/release/package

.PHONY: release
release: build/release/CMakeCache.txt
	cmake --build build/release -j12

build/release/CMakeCache.txt:
	cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release


############ Docs
.PHONY: docs
docs: build/docs/CMakeCache.txt
	cmake --build build/docs --target docs

build/docs/CMakeCache.txt:
	cmake -S . -B build/docs -DNUMSTORE_DOCS=ON


############ Code Coverage
# Filters and the assert/harness exclusions live in gcovr.cfg - see that file
# for what is left out of the ratio and why. The search path stays here
# because it is the one thing a CI job might point somewhere else.
GCOVR = gcovr --config gcovr.cfg build/coverage --print-summary

.PHONY: coverage
coverage: build/coverage/html/index.html

build/coverage/CMakeCache.txt:
	cmake -S . -B build/coverage \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_C_FLAGS="--coverage -O0" \
		-DCMAKE_EXE_LINKER_FLAGS="--coverage" \
		-DCMAKE_SHARED_LINKER_FLAGS="--coverage"

build/coverage/bin/ns_simtest: build/coverage/bin/unit_tests
build/coverage/bin/unit_tests: build/coverage/CMakeCache.txt
	cmake --build build/coverage -j12

build/coverage/.stamp: build/coverage/bin/unit_tests build/coverage/bin/ns_simtest
	find build/coverage -name "*.gcda" -delete
	./build/coverage/bin/unit_tests $(SEED)
	./build/coverage/bin/ns_simtest \
		--dbname build/coverage/simtest.db \
		--duration 5 \
		--seed $(SEED)
	./build/coverage/bin/smfile_sample1_basic_crud
	./build/coverage/bin/smfile_sample2_transactions
	./build/coverage/bin/smfile_sample3_stride
	./build/coverage/bin/smfile_sample4_rollback_commit
	./build/coverage/bin/ns_sample1_basic_crud
	touch build/coverage/.stamp

build/coverage/html/index.html: build/coverage/.stamp
	rm -rf build/coverage/html
	mkdir -p build/coverage/html
	$(GCOVR) --html-details build/coverage/html/index.html

.PHONY: coverage-xml
coverage-xml: build/coverage/.stamp
	mkdir -p build/coverage/report
	$(GCOVR) --cobertura build/coverage/report/coverage.xml \
		--cobertura-pretty \
		--txt build/coverage/report/coverage.txt

# Runs after coverage-xml so the report is on disk and uploadable even when
# the ratio has regressed -- that is exactly when it's worth reading.
.PHONY: coverage-check
coverage-check: build/coverage/.stamp
	$(GCOVR) \
		--fail-under-line $(COV_MIN_LINE) \
		--fail-under-branch $(COV_MIN_BRANCH)

COV_MIN_LINE   ?= 80
COV_MIN_BRANCH ?= 45

# Sub-makes rather than prerequisites: the gate has to run last even under
# -j, and the XML has to be on disk before the gate can fail the build.
.PHONY: ci-coverage
ci-coverage:
	$(MAKE) coverage
	$(MAKE) coverage-xml
	$(MAKE) coverage-check

############ Leak checking

build/memcheck/CMakeCache.txt:
	cmake -S . -B build/memcheck \
		-DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_C_FLAGS="-g3 -O0 -fno-inline -fno-omit-frame-pointer -Wall -Wextra -Werror"

build/memcheck/bin/ns_simtest: build/memcheck/bin/unit_tests
build/memcheck/bin/unit_tests: build/memcheck/CMakeCache.txt
	cmake --build build/memcheck -j12


############ Valgrind (linux)

VALGRIND = valgrind \
	--tool=memcheck \
	--error-exitcode=1 \
	--leak-check=full \
	--show-leak-kinds=all \
	--errors-for-leak-kinds=all \
	--track-origins=yes \
	--track-fds=yes \
	--trace-children=yes \
	--undef-value-errors=yes \
	--expensive-definedness-checks=yes \
	--partial-loads-ok=no \
	--malloc-fill=0xab \
	--free-fill=0xcd \
	--keep-debuginfo=yes \
	--read-var-info=yes \
	--num-callers=50 \
	--fair-sched=yes \
	--gen-suppressions=all

.PHONY: valgrind
valgrind: valgrind-unit valgrind-simtest

.PHONY: valgrind-unit
valgrind-unit: build/memcheck/bin/unit_tests
	$(VALGRIND) ./build/memcheck/bin/unit_tests 123142

.PHONY: valgrind-simtest
valgrind-simtest: build/memcheck/bin/ns_simtest
	$(VALGRIND) ./build/memcheck/bin/ns_simtest --dbname simtest.db --duration 5 --seed 123142


############ Leaks (macos)

LEAKS = MallocStackLogging=1 \
	MallocStackLoggingNoCompact=1 \
	MallocScribble=1 \
	MallocPreScribble=1 \
	MallocGuardEdges=1 \
	leaks --atExit --list --fullStacks --

.PHONY: leaks
leaks: leaks-unit leaks-simtest

.PHONY: leaks-unit
leaks-unit: build/memcheck/bin/unit_tests
	$(LEAKS) ./build/memcheck/bin/unit_tests 123142

.PHONY: leaks-simtest
leaks-simtest: build/memcheck/bin/ns_simtest
	$(LEAKS) ./build/memcheck/bin/ns_simtest --dbname simtest.db --duration 5 --seed 123142


#############################################
### pynumstore

PKG_PY_SRCS     := $(shell find pynumstore -name '*.py' -o -name "*.pyi")
PKG_NATIVE_SRCS := $(shell find . \( -name '*.c' -o -name '*.h' -o -name '*.cmake' -o -name 'CMakeLists.txt' \) -not -path '*build*')

build/python/.installed: $(PKG_PY_SRCS) $(PKG_NATIVE_SRCS) pyproject.toml version.txt
	python3 -m pip install '.[test]'
	mkdir -p build/python
	touch build/python/.installed
 
.PHONY: python-install
python-install: build/python/.installed
 
.PHONY: python-reinstall
python-reinstall:
	rm -rf build/python build/wheel
	$(MAKE) python-install
 
# Run from inside pytest/ so the tests import the installed package,
# not the ./pynumstore source directory.
.PHONY: test-python
test-python: python-install
	cd pytest && python3 -m pytest -ra --tb=short
 
.PHONY: wheel
wheel:
	python3 -m build --wheel . --outdir dist
 
.PHONY: sdist
sdist:
	python3 -m build --sdist . --outdir dist

#############################################
### Housekeeping
.PHONY: format
EXCLUDE := -not -path '*build*'

C_SRCS     := $(shell find . -name '*.c' $(EXCLUDE))
H_SRCS     := $(shell find . -name '*.h' $(EXCLUDE))
CMAKE_SRCS := $(shell find . \( -name '*.cmake' -o -name 'CMakeLists.txt' \) $(EXCLUDE))
PY_SRCS    := $(shell find . -name '*.py' $(EXCLUDE))

format:
	-clang-format -i $(C_SRCS) $(H_SRCS)
	-gersemi -i --no-warn-about-unknown-commands $(CMAKE_SRCS)
	-ruff format $(PY_SRCS)

.PHONY: clean 
clean: clean-c clean-python

.PHONY: clean-common
clean-common:
	rm -rf build build_coverage docs/build docs/api 
	find . -type f -name '*.db'     -delete
	find . -type f -name '*.nsdb'   -delete
	find . -type f -name '*.wal'    -delete
	find . -type f -name 'testdb*'  -delete
	find . -type f -name 'vgcore.*' -delete
	find . -type f -name gmon.out   -delete
	rm -f foo test ns_simtest

.PHONY: clean-c
clean-c: clean-common
	find . -type f -name '*.o'    -delete
	find . -type f -name '*.d'    -delete
	find . -type f -name '*.gcda' -delete
	find . -type f -name '*.gcno' -delete
	find . -type f -name '*.gcov' -delete

.PHONY: clean-python
clean-python: clean-common
	rm -rf dist wheelhouse
	find . -type d -name __pycache__   -prune -exec rm -rf {} +
	find . -type d -name '*.egg-info'  -prune -exec rm -rf {} +
	find . -type d -name .pytest_cache -prune -exec rm -rf {} +
	find . -type d -name .ruff_cache   -prune -exec rm -rf {} +
