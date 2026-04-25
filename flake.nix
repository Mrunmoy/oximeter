{
  description = "OxiNode — MAX30102 pulse oximeter (RP2040-Zero + ESP32-S3-Zero)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # Pico SDK with submodules — the nixpkgs `pico-sdk` package strips
    # tinyusb/cyw43-driver/btstack which we need for USB-CDC.
    pico-sdk-src = {
      type = "git";
      url = "https://github.com/raspberrypi/pico-sdk.git";
      ref = "refs/tags/2.2.0";
      submodules = true;
      flake = false;
    };
  };

  outputs = { self, nixpkgs, flake-utils, pico-sdk-src }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        commonHostPackages = with pkgs; [
          # Build system
          cmake
          ninja
          pkg-config
          git
          gnumake

          # Host-side tests (driver core + DSP)
          gcc
          gtest
          gcovr
          lcov

          # Serial / flashing utilities
          picocom
          minicom
          dfu-util
          usbutils

          # Python — desktop TUI client + asset/codegen scripts
          (python3.withPackages (ps: with ps; [
            pyserial
            rich
            textual
            click
            pytest
            pyyaml
          ]))

          # Datasheet / curl
          curl
          jq
        ];

        rp2040Packages = with pkgs; [
          gcc-arm-embedded
          picotool
          openocd-rp2040
        ];

        # ESP-IDF intentionally NOT vendored into the Nix store: the
        # installer stages ~400 MB of vendor blobs that violate the Nix
        # purity model and break frequently. Use the Docker path or
        # `firmware/esp32s3/env.sh` (matches esp-clock convention) for
        # the ESP32-S3 build. The Nix shell still supplies all the host
        # tooling needed *around* idf.py.
      in
      {
        devShells.default = pkgs.mkShell {
          name = "oxinode";

          packages = commonHostPackages ++ rp2040Packages;

          shellHook = ''
            export PICO_SDK_PATH=${pico-sdk-src}
            export PICO_BOARD=pico
            export OXINODE_ROOT=$(pwd)

            echo "──────────────────────────────────────────────────────"
            echo " OxiNode dev shell — RP2040 prototype path is primary"
            echo "──────────────────────────────────────────────────────"
            echo "  arm-none-eabi-gcc : $(arm-none-eabi-gcc --version | head -1)"
            echo "  picotool          : $(picotool version 2>&1 | head -1)"
            echo "  cmake             : $(cmake --version | head -1)"
            echo "  PICO_SDK_PATH     : $PICO_SDK_PATH"
            echo ""
            echo " Build :  ./scripts/build.sh {rp2040 | host-tests | esp32s3}"
            echo " Flash :  ./scripts/flash.sh rp2040"
            echo " ESP32 :  bring up via firmware/esp32s3/env.sh + idf.py"
            echo "          (or ./docker/docker-build.sh esp32s3)"
          '';
        };
      });
}
