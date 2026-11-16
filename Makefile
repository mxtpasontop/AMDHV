# ============================================================================
# AMDHV.kext Makefile
# Builds:
#   1. build/AMDHV.kext  - macOS Kernel Extension (x86_64 Lilu plugin)
#   2. build/amdhv_tests - Userspace Unit Test Suite
# ============================================================================

CXX       := xcrun clang++
CC        := xcrun clang
SDKROOT   := $(shell xcrun --sdk macosx --show-sdk-path)
KERNEL_HEADERS := vendor/MacKernelSDK/Headers
LILU_HEADERS := vendor/Lilu/Lilu

BUILD_DIR := build
KEXT_DIR  := $(BUILD_DIR)/AMDHV.kext
KEXT_MACOS:= $(KEXT_DIR)/Contents/MacOS
KEXT_BIN  := $(KEXT_MACOS)/AMDHV
TEST_BIN  := $(BUILD_DIR)/amdhv_tests
SMOKE_BIN := $(BUILD_DIR)/hv_smoke_test

COMMON_Search := -I. -I$(BUILD_DIR)
STRICT_WARNINGS := $(shell python3 tools/compiler_warnings.py $(CXX) || echo AMDHV_COMPILER_CHECK_FAILED)
ifneq ($(filter AMDHV_COMPILER_CHECK_FAILED,$(STRICT_WARNINGS)),)
$(error Compiler warning-policy discovery failed)
endif
HEADERS := $(wildcard src/*.hpp src/*.h Headers/Lilu/*.hpp) $(BUILD_DIR)/BuildInfo.hpp $(BUILD_DIR)/CompatibilityInfo.hpp $(BUILD_DIR)/ABIInfo.hpp

KEXT_CXXFLAGS := \
	-target x86_64-apple-macos12.0 \
	-std=c++17 \
	-O2 \
	-Wall -Wextra -Werror -Wframe-larger-than=4096 \
	$(STRICT_WARNINGS) \
	-fno-builtin -fno-common -fno-exceptions -fno-rtti -fno-stack-protector \
	-mkernel -mno-red-zone \
	-DAMDHV_KERNEL_BUILD=1 \
	-DPRODUCT_NAME=AMDHV -DMODULE_VERSION=1.0.0 -DLILU_CUSTOM_IOKIT_INIT=1 -DLILU_CUSTOM_KMOD_INIT=1 \
	-isystem $(KERNEL_HEADERS) -isystem $(LILU_HEADERS) \
	-F$(SDKROOT)/System/Library/Frameworks \
	$(COMMON_Search)

KEXT_LDFLAGS := \
	-target x86_64-apple-macos12.0 \
	-Wl,-kext \
	-nostdlib \
	-Lvendor/MacKernelSDK/Library/x86_64 -lkmod -lcc_kext \
	-Wl,-undefined,dynamic_lookup

TEST_EXTRA_FLAGS :=
TEST_CXXFLAGS := \
	-std=c++17 \
	-O2 \
	-Wall -Wextra -Werror \
	$(COMMON_Search) $(TEST_EXTRA_FLAGS)

KEXT_SRCS := \
	src/SVMTransition.cpp \
	src/SpaceLifecycle.cpp \
	src/VcpuLifecycle.cpp \
	src/TrapAdapter.cpp \
	src/EPTSource.cpp \
	src/GuestMemory.cpp \
	src/TrapRecovery.cpp \
	src/FaultMemory.cpp \
	src/XNUPlatform.cpp \
	src/SVMCPU.cpp \
	src/Memory.cpp \
	src/InstructionDecoder.cpp \
	src/SoftVMCS.cpp \
	src/NPTManager.cpp \
	src/VMCBTranslator.cpp \
	src/AMDHV.cpp

KEXT_ASM := \
	src/SVMTransition.S \
	src/FaultProbe.S \
	src/SVMEntry.S

TEST_SRCS := \
	src/SVMTransition.cpp \
	src/SpaceLifecycle.cpp \
	src/VcpuLifecycle.cpp \
	src/TrapAdapter.cpp \
	src/EPTSource.cpp \
	src/GuestMemory.cpp \
	src/TrapRecovery.cpp \
	src/FaultMemory.cpp \
	src/XNUPlatform.cpp \
	src/SVMCPU.cpp \
	src/Memory.cpp \
	src/InstructionDecoder.cpp \
	src/SoftVMCS.cpp \
	src/NPTManager.cpp \
	src/VMCBTranslator.cpp \
	tests/test_main.cpp

.PHONY: all kext test hv_smoke_test clean

all: kext test hv_smoke_test

kext: verify-dependencies $(KEXT_BIN)

KEXT_OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(KEXT_SRCS)) $(patsubst src/%.S,$(BUILD_DIR)/%Asm.o,$(KEXT_ASM))

$(BUILD_DIR)/%.o: src/%.cpp $(HEADERS) Makefile | verify-dependencies
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(KEXT_CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%Asm.o: src/%.S src/TransitionLayout.h Makefile | verify-dependencies
	@mkdir -p $(BUILD_DIR)
	$(CC) -target x86_64-apple-macos12.0 -c $< -o $@

$(KEXT_BIN): $(KEXT_OBJECTS) $(HEADERS) Info.plist Makefile dependencies.lock.json $(wildcard config/*.json tools/*.py) | verify-dependencies
	@mkdir -p $(KEXT_MACOS)
	$(CXX) $(KEXT_LDFLAGS) $(KEXT_OBJECTS) -o $(BUILD_DIR)/AMDHV.linked
	python3 tools/package_kext.py $(BUILD_DIR)/AMDHV.linked Info.plist $(KEXT_DIR)

test: $(TEST_BIN) $(BUILD_DIR)/amdhv_hook_tests $(BUILD_DIR)/amdhv_hardening_tests $(BUILD_DIR)/amdhv_runtime_tests $(BUILD_DIR)/amdhv_adapter_tests
	./$(TEST_BIN)
	./$(BUILD_DIR)/amdhv_hook_tests
	./$(BUILD_DIR)/amdhv_hardening_tests
	./$(BUILD_DIR)/amdhv_runtime_tests
	./$(BUILD_DIR)/amdhv_adapter_tests

$(TEST_BIN): $(TEST_SRCS) src/SVMTransition.S src/FaultProbe.S $(HEADERS) tests/PhysicalFixture.hpp Makefile
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(TEST_CXXFLAGS) $(TEST_SRCS) src/SVMTransition.S src/FaultProbe.S -o $(TEST_BIN)

$(BUILD_DIR)/amdhv_hook_tests: tests/SpaceHookFixture.hpp tests/PhysicalFixture.hpp tests/test_hooks.cpp src/AMDHV.cpp $(filter-out tests/test_main.cpp,$(TEST_SRCS)) $(HEADERS) $(KEXT_ASM) Makefile
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(TEST_CXXFLAGS) tests/test_hooks.cpp $(filter-out tests/test_main.cpp,$(TEST_SRCS)) $(KEXT_ASM) -o $@

hv_smoke_test: $(SMOKE_BIN)

$(SMOKE_BIN): tests/hv_smoke_test.c tests/hv_entitlements.plist
	@mkdir -p $(BUILD_DIR)
	$(CC) -target x86_64-apple-macos12.0 -O2 -Wall -Wextra -framework Hypervisor -Wno-deprecated-declarations tests/hv_smoke_test.c -o $(SMOKE_BIN)
	codesign --entitlements tests/hv_entitlements.plist --force -s - $(SMOKE_BIN)

clean:
	python3 tools/clean.py $(BUILD_DIR)

$(BUILD_DIR)/amdhv_hardening_tests: tests/test_hardening.cpp src/SVMTransition.S src/FaultProbe.S $(filter-out tests/test_main.cpp,$(TEST_SRCS)) $(HEADERS) tests/PhysicalFixture.hpp Makefile
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(TEST_CXXFLAGS) tests/test_hardening.cpp $(filter-out tests/test_main.cpp,$(TEST_SRCS)) src/SVMTransition.S src/FaultProbe.S -o $@

.PHONY: sanitize thread-sanitize check python-test
CORE_SRCS := $(filter-out tests/test_main.cpp,$(TEST_SRCS))
sanitize: $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	@set -e; for suite in main hardening runtime hooks adapters; do \
	  extra="src/SVMTransition.S src/FaultProbe.S"; if [ "$$suite" = hooks ]; then extra="$(KEXT_ASM)"; fi; \
	  $(CXX) $(TEST_CXXFLAGS) -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_$$suite.cpp $(CORE_SRCS) $$extra -o $(BUILD_DIR)/sanitized_$$suite; \
	  ./$(BUILD_DIR)/sanitized_$$suite; \
	done

thread-sanitize: $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(TEST_CXXFLAGS) -g -pthread -fsanitize=thread tests/test_runtime.cpp $(CORE_SRCS) src/SVMTransition.S src/FaultProbe.S -o $(BUILD_DIR)/thread_sanitized_runtime
	./$(BUILD_DIR)/thread_sanitized_runtime

python-test:
	python3 -m unittest discover -s tests -p 'test_*.py' -v

check: test python-test
	zsh -n test_after_reboot.sh
	plutil -lint Info.plist tests/hv_entitlements.plist

.PHONY: FORCE
FORCE:
$(BUILD_DIR)/BuildInfo.hpp: FORCE
	@python3 tools/build_identity.py $@

.PHONY: verify-dependencies
verify-dependencies:
	@python3 tools/dependencies.py

$(BUILD_DIR)/CompatibilityInfo.hpp: config/compatibility.json tools/compatibility_header.py
	@python3 tools/compatibility_header.py $@

$(BUILD_DIR)/amdhv_runtime_tests: tests/test_runtime.cpp tests/VcpuFixture.hpp tests/TrapFixture.hpp src/SVMTransition.S src/FaultProbe.S tests/PhysicalFixture.hpp $(filter-out tests/test_main.cpp,$(TEST_SRCS)) $(HEADERS) Makefile
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(TEST_CXXFLAGS) -pthread tests/test_runtime.cpp $(filter-out tests/test_main.cpp,$(TEST_SRCS)) src/SVMTransition.S src/FaultProbe.S -o $@

$(BUILD_DIR)/amdhv_adapter_tests: tests/test_adapters.cpp $(CORE_SRCS) $(HEADERS) src/SVMTransition.S src/FaultProbe.S Makefile
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(TEST_CXXFLAGS) tests/test_adapters.cpp $(CORE_SRCS) src/SVMTransition.S src/FaultProbe.S -o $@

$(BUILD_DIR)/ABIInfo.hpp: config/abi.json tools/abi_header.py tools/abi_profile.py
	@python3 tools/abi_header.py $@

.PHONY: emulate-transition
emulate-transition:
	python3 tools/emulate_transition.py
