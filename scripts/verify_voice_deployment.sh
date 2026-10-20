#!/bin/bash
# verify_voice_deployment.sh
# 离线语音交互板端部署验证脚本
#
# Usage:
#   chmod +x scripts/verify_voice_deployment.sh
#   ./scripts/verify_voice_deployment.sh

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Helper functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

check_success() {
    if [ $? -eq 0 ]; then
        log_info "$1"
    else
        log_error "$2"
        return 1
    fi
}

# ============================================================
# Step 1: Check sherpa-onnx library installation
# ============================================================
echo ""
echo "=========================================="
echo "Step 1: Checking sherpa-onnx library"
echo "=========================================="

if ldconfig -p | grep -q "libsherpa-onnx-c-api"; then
    log_info "sherpa-onnx C API library found"
    ldconfig -p | grep "libsherpa-onnx-c-api"
else
    log_warn "sherpa-onnx C API library not found"
    log_info "Installing sherpa-onnx..."
    echo ""
    echo "Please install sherpa-onnx on the board:"
    echo "  # Method 1: Install from pre-built packages"
    echo "  sudo apt-get update"
    echo "  sudo apt-get install -y libsherpa-onnx-dev"
    echo ""
    echo "  # Method 2: Build from source"
    echo "  git clone https://github.com/k2-fsa/sherpa-onnx.git"
    echo "  cd sherpa-onnx"
    echo "  mkdir build && cd build"
    echo "  cmake -DCMAKE_BUILD_TYPE=Release .."
    echo "  make -j\$(nproc)"
    echo "  sudo make install"
    echo ""
fi

# ============================================================
# Step 2: Check model files
# ============================================================
echo ""
echo "=========================================="
echo "Step 2: Checking model files"
echo "=========================================="

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
CONFIG_DIR="$PROJECT_DIR/config"

# Check DeepSeek .rkllm model
if [ -f "$CONFIG_DIR/DeepSeek-R1-Distill-Qwen-1.5B_W8A8_RK3588.rkllm" ]; then
    SIZE=$(du -h "$CONFIG_DIR/DeepSeek-R1-Distill-Qwen-1.5B_W8A8_RK3588.rkllm" | cut -f1)
    log_info "DeepSeek .rkllm model found ($SIZE)"
else
    log_error "DeepSeek .rkllm model not found at $CONFIG_DIR/"
fi

# Check SummerTTS .bin model
if [ -f "$CONFIG_DIR/single_speaker_fast.bin" ]; then
    SIZE=$(du -h "$CONFIG_DIR/single_speaker_fast.bin" | cut -f1)
    log_info "SummerTTS .bin model found ($SIZE)"
else
    log_error "SummerTTS .bin model not found at $CONFIG_DIR/"
fi

# Check sherpa-onnx ASR models
if [ -d "$CONFIG_DIR/sherpa-onnx-model" ]; then
    log_info "sherpa-onnx model directory found"
    ls -la "$CONFIG_DIR/sherpa-onnx-model/"*.onnx 2>/dev/null || log_warn "No .onnx files found"
else
    log_error "sherpa-onnx model directory not found"
fi

# ============================================================
# Step 3: Check librkllmrt.so
# ============================================================
echo ""
echo "=========================================="
echo "Step 3: Checking librkllmrt.so"
echo "=========================================="

if ldconfig -p | grep -q "librkllmrt"; then
    log_info "librkllmrt.so found"
else
    log_warn "librkllmrt.so not found in system library path"
    log_info "Checking if librkllmrt.so exists in config directory..."
    if [ -f "$CONFIG_DIR/librkllmrt.so" ]; then
        log_info "Found in config directory, installing..."
        sudo cp "$CONFIG_DIR/librkllmrt.so" /usr/lib/aarch64-linux-gnu/
        sudo ldconfig
        log_info "librkllmrt.so installed"
    else
        log_error "librkllmrt.so not found"
    fi
fi

# ============================================================
# Step 4: Check ALSA audio devices
# ============================================================
echo ""
echo "=========================================="
echo "Step 4: Checking ALSA audio devices"
echo "=========================================="

if command -v arecord &> /dev/null; then
    log_info "arecord available"
    echo ""
    echo "Available capture devices:"
    arecord -l 2>/dev/null || log_warn "No capture devices found"
else
    log_warn "arecord not installed"
    echo "Install with: sudo apt-get install -y alsa-utils"
fi

if command -v aplay &> /dev/null; then
    log_info "aplay available"
    echo ""
    echo "Available playback devices:"
    aplay -l 2>/dev/null || log_warn "No playback devices found"
else
    log_warn "aplay not installed"
fi

# ============================================================
# Step 5: Build LLM/TTS executables
# ============================================================
echo ""
echo "=========================================="
echo "Step 5: Building LLM/TTS executables"
echo "=========================================="

VOICE_DIR="$PROJECT_DIR/voice"
BUILD_DIR="$PROJECT_DIR/build_voice"

if [ -f "$VOICE_DIR/CMakeLists.txt" ]; then
    log_info "Building voice services..."
    mkdir -p "$BUILD_DIR"
    cd "$BUILD_DIR"
    cmake "$VOICE_DIR" -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc)
    check_success "Voice services built successfully" "Failed to build voice services"
else
    log_error "CMakeLists.txt not found in $VOICE_DIR"
fi

# ============================================================
# Step 6: Enable voice in configuration
# ============================================================
echo ""
echo "=========================================="
echo "Step 6: Enabling voice configuration"
echo "=========================================="

VOICE_CONF="$CONFIG_DIR/voice_settings.conf"
ASR_CONF="$CONFIG_DIR/asr_settings_offline.conf"

if [ -f "$VOICE_CONF" ]; then
    # Backup original config
    cp "$VOICE_CONF" "${VOICE_CONF}.bak"
    
    # Enable voice
    sed -i 's/^voice_enabled=false/voice_enabled=true/' "$VOICE_CONF"
    log_info "Voice enabled in $VOICE_CONF"
else
    log_error "Voice config not found: $VOICE_CONF"
fi

if [ -f "$ASR_CONF" ]; then
    # Backup original config
    cp "$ASR_CONF" "${ASR_CONF}.bak"
    
    # Enable ASR
    sed -i 's/^asr_enabled=false/asr_enabled=true/' "$ASR_CONF"
    log_info "ASR enabled in $ASR_CONF"
else
    log_error "ASR config not found: $ASR_CONF"
fi

# ============================================================
# Step 7: Verify configuration
# ============================================================
echo ""
echo "=========================================="
echo "Step 7: Verifying configuration"
echo "=========================================="

echo ""
echo "Current voice_settings.conf:"
grep -v "^#" "$VOICE_CONF" | grep -v "^$"
echo ""
echo "Current asr_settings_offline.conf:"
grep -v "^#" "$ASR_CONF" | grep -v "^$"

# ============================================================
# Step 8: Test complete pipeline (optional)
# ============================================================
echo ""
echo "=========================================="
echo "Step 8: Pipeline test instructions"
echo "=========================================="

echo ""
echo "To test the complete voice pipeline:"
echo ""
echo "1. Start the VehicleTerminal application:"
echo "   cd <PROJECT_DIR>/VehicleTerminal"
echo "   ./run_hdmi.sh"
echo ""
echo "2. Or start voice services manually:"
echo "   # Terminal 1: Start LLM service"
echo "   cd <PROJECT_DIR>/build_voice"
echo "   ./llm_service ../config/DeepSeek-R1-Distill-Qwen-1.5B_W8A8_RK3588.rkllm"
echo ""
echo "   # Terminal 2: Start TTS service"
echo "   cd <PROJECT_DIR>/build_voice"
echo "   ./tts_service ../config/single_speaker_fast.bin"
echo ""
echo "3. Speak into the microphone and observe:"
echo "   - ASR recognizes speech and displays text"
echo "   - LLM processes the text and generates response"
echo "   - TTS synthesizes speech and plays through speaker"
echo ""

# ============================================================
# Summary
# ============================================================
echo ""
echo "=========================================="
echo "Deployment Verification Summary"
echo "=========================================="
echo ""
echo "Status:"
echo "  [1] sherpa-onnx library: $(ldconfig -p | grep -q 'libsherpa-onnx-c-api' && echo 'OK' || echo 'NOT FOUND')"
echo "  [2] DeepSeek .rkllm model: $([ -f "$CONFIG_DIR/DeepSeek-R1-Distill-Qwen-1.5B_W8A8_RK3588.rkllm" ] && echo 'OK' || echo 'NOT FOUND')"
echo "  [3] SummerTTS .bin model: $([ -f "$CONFIG_DIR/single_speaker_fast.bin" ] && echo 'OK' || echo 'NOT FOUND')"
echo "  [4] sherpa-onnx ASR models: $([ -d "$CONFIG_DIR/sherpa-onnx-model" ] && echo 'OK' || echo 'NOT FOUND')"
echo "  [5] librkllmrt.so: $(ldconfig -p | grep -q 'librkllmrt' && echo 'OK' || echo 'NOT FOUND')"
echo "  [6] voice_enabled: $(grep '^voice_enabled=true' "$VOICE_CONF" > /dev/null 2>&1 && echo 'YES' || echo 'NO')"
echo "  [7] asr_enabled: $(grep '^asr_enabled=true' "$ASR_CONF" > /dev/null 2>&1 && echo 'YES' || echo 'NO')"
echo ""
echo "Next steps:"
echo "  1. If any component is missing, install it first"
echo "  2. Run the VehicleTerminal application"
echo "  3. Test the complete voice pipeline"
echo ""
