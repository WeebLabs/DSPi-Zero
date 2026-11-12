# Bare-metal DSPi platform layer for the Raspberry Pi Zero (BCM2835) and Zero 2 W (BCM2837, 32-bit).
# Standalone build, not part of firmware/CMakeLists.txt. One build directory per board:
#   cmake -S firmware/PiZero -B firmware/PiZero/build -G Ninja && cmake --build firmware/PiZero/build
#   cmake -S firmware/PiZero -B firmware/PiZero/build-zero2 -G Ninja -DPIZERO_BOARD=zero2 && cmake --build ...
cmake_minimum_required(VERSION 3.13)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_LIST_DIR}/cmake/arm-none-eabi.cmake)
endif()

project(dspi_pizero C ASM)

# Optimisation is set per target below, so drop CMake's per-build-type -O flags.
set(CMAKE_C_FLAGS_RELEASE "")
set(CMAKE_ASM_FLAGS_RELEASE "")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Adafruit broadcom-peripherals, fetched by: python3 tools/get_deps.py broadcom_32bit (inside TinyUSB).
set(BCM_DIR ${CMAKE_CURRENT_LIST_DIR}/../../DSPi/firmware/pico-sdk/lib/tinyusb/hw/mcu/broadcom)
if(NOT EXISTS ${BCM_DIR}/broadcom/vcmailbox.c)
    message(FATAL_ERROR "broadcom-peripherals missing. Run: cd DSPi/firmware/pico-sdk/lib/tinyusb && python3 tools/get_deps.py broadcom_32bit")
endif()

set(PIZERO_BOARD zero CACHE STRING "Target board: zero (BCM2835) or zero2 (BCM2837, AArch32)")
set_property(CACHE PIZERO_BOARD PROPERTY STRINGS zero zero2)
if(PIZERO_BOARD STREQUAL "zero")
    set(CPU_FLAGS -mcpu=arm1176jzf-s -mfpu=vfp -mfloat-abi=hard -marm)
    set(BCM_VERSION 2835)
elseif(PIZERO_BOARD STREQUAL "zero2")
    set(CPU_FLAGS -mcpu=cortex-a53 -mfpu=neon-fp-armv8 -mfloat-abi=hard -marm)
    set(BCM_VERSION 2837)
else()
    message(FATAL_ERROR "PIZERO_BOARD must be zero or zero2, not ${PIZERO_BOARD}")
endif()
message(STATUS "PiZero board: ${PIZERO_BOARD} (BCM${BCM_VERSION})")

# With the MMU off every data access is strongly ordered, where unaligned accesses fault.
set(COMMON_FLAGS ${CPU_FLAGS} -ffreestanding -fno-common -ffunction-sections -fdata-sections -mno-unaligned-access)

# Built at -O0, the way TinyUSB's broadcom_32bit BSP builds and tests it. We use the GPIO helpers,
# the cache helpers TinyUSB's DWC2 glue links against, and the register headers. mbox.c replaces vcmailbox.c.
if(BCM_VERSION EQUAL 2835)
    set(BCM_CACHES ${BCM_DIR}/broadcom/caches.c)
else()
    set(BCM_CACHES ${CMAKE_CURRENT_LIST_DIR}/caches_v7.c)
endif()
add_library(bcm_peripherals STATIC
    ${BCM_DIR}/broadcom/gpio.c
    ${BCM_CACHES}
)
target_include_directories(bcm_peripherals PUBLIC ${BCM_DIR})
target_compile_definitions(bcm_peripherals PUBLIC BCM_VERSION=${BCM_VERSION})
target_compile_options(bcm_peripherals PRIVATE ${COMMON_FLAGS} -O0 -std=c17 -w)

set(PLATFORM_SRC startup.S board.c uart.c mbox.c bootcheck.c)

# Each milestone is its own image. <target>.img is the raw image the GPU firmware loads at 0x8000.
# The latest milestone (SDCARD_TARGET) is also staged as sdcard/kernel.img next to config.txt.
set(SDCARD_TARGET pizero_z2)
# An optional second argument names another linker script (the loader's); both INCLUDE pizero_sections.ld.
function(pizero_image target)
    set(ldscript pizero.ld)
    if(ARGC GREATER 1)
        set(ldscript ${ARGV1})
    endif()
    target_compile_options(${target} PRIVATE ${COMMON_FLAGS} -O2 -g -std=c17 -Wall -Wextra)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/loader)
    target_link_libraries(${target} PRIVATE bcm_peripherals)
    target_link_options(${target} PRIVATE
        ${CPU_FLAGS}
        -nostartfiles
        --specs=nano.specs
        --specs=nosys.specs
        -L${CMAKE_CURRENT_LIST_DIR}
        -T${CMAKE_CURRENT_LIST_DIR}/${ldscript}
        -Wl,--gc-sections
        -Wl,--no-warn-rwx-segments
        -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${target}.map
    )
    set_target_properties(${target} PROPERTIES SUFFIX ".elf"
        LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/${ldscript};${CMAKE_CURRENT_LIST_DIR}/pizero_sections.ld")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${target}> ${CMAKE_CURRENT_BINARY_DIR}/${target}.img
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${target}>
        VERBATIM
    )
    if(target STREQUAL SDCARD_TARGET)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/sdcard
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/${target}.img ${CMAKE_CURRENT_BINARY_DIR}/sdcard/kernel.img
            COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/boot/config.txt ${CMAKE_CURRENT_BINARY_DIR}/sdcard/config.txt
            VERBATIM
        )
    endif()
endfunction()

# Z0: LED blink and UART heartbeat.
add_executable(pizero_z0 ${PLATFORM_SRC} main_z0.c)
pizero_image(pizero_z0)

# Z1: TinyUSB vendor echo on the DWC2 controller, polled from the main loop.
set(TUSB_DIR ${CMAKE_CURRENT_LIST_DIR}/../../DSPi/firmware/pico-sdk/lib/tinyusb/src)
add_library(tinyusb_z1 STATIC
    ${TUSB_DIR}/tusb.c
    ${TUSB_DIR}/common/tusb_fifo.c
    ${TUSB_DIR}/device/usbd.c
    ${TUSB_DIR}/device/usbd_control.c
    ${TUSB_DIR}/class/vendor/vendor_device.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dcd_dwc2.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dwc2_common.c
)
target_include_directories(tinyusb_z1 PUBLIC ${TUSB_DIR} ${CMAKE_CURRENT_LIST_DIR}/z1)
target_link_libraries(tinyusb_z1 PUBLIC bcm_peripherals)
target_compile_options(tinyusb_z1 PRIVATE ${COMMON_FLAGS} -O2 -g -std=c17 -Wall)

add_executable(pizero_z1 ${PLATFORM_SRC} z1/main_z1.c z1/usb_descriptors.c)
target_link_libraries(pizero_z1 PRIVATE tinyusb_z1)
pizero_image(pizero_z1)

# Z2: DSPi's own UAC1 descriptors (firmware/DSPi/usb_descriptors.c, stereo alts only, product string renamed)
# with a cut-down copy of its class driver. z2/shim stands in for the one Pico SDK header it includes.
set(DSPI_DIR ${CMAKE_CURRENT_LIST_DIR}/../../DSPi/firmware/DSPi)   # the DSPi submodule
add_library(tinyusb_z2 STATIC
    ${TUSB_DIR}/tusb.c
    ${TUSB_DIR}/common/tusb_fifo.c
    ${TUSB_DIR}/device/usbd.c
    ${TUSB_DIR}/device/usbd_control.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dcd_dwc2.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dwc2_common.c
)
target_include_directories(tinyusb_z2 PUBLIC ${TUSB_DIR} ${CMAKE_CURRENT_LIST_DIR}/z2)
target_link_libraries(tinyusb_z2 PUBLIC bcm_peripherals)
target_compile_options(tinyusb_z2 PRIVATE ${COMMON_FLAGS} -O2 -g -std=c17 -Wall)

# The product string becomes "Weeb Labs DSPi Zero". The copy is regenerated whenever the DSPi file changes,
# and configuring fails if the string it replaces is no longer there.
set(DSPI_PRODUCT_FROM "\"Weeb Labs DSPi\",")
set(DSPI_PRODUCT_TO "\"Weeb Labs DSPi Zero\",")
file(READ ${DSPI_DIR}/usb_descriptors.c DSPI_DESC_SRC)
string(FIND "${DSPI_DESC_SRC}" "${DSPI_PRODUCT_FROM}" DSPI_PRODUCT_POS)
if(DSPI_PRODUCT_POS EQUAL -1)
    message(FATAL_ERROR "DSPi product string not found in ${DSPI_DIR}/usb_descriptors.c")
endif()
string(REPLACE "${DSPI_PRODUCT_FROM}" "${DSPI_PRODUCT_TO}" DSPI_DESC_SRC "${DSPI_DESC_SRC}")
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/z2/usb_descriptors_zero.c CONTENT "${DSPI_DESC_SRC}" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DSPI_DIR}/usb_descriptors.c)

add_library(dspi_descriptors_z2 STATIC ${CMAKE_CURRENT_BINARY_DIR}/z2/usb_descriptors_zero.c)
target_include_directories(dspi_descriptors_z2 BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/z2 ${CMAKE_CURRENT_LIST_DIR}/z2/shim)
target_include_directories(dspi_descriptors_z2 PRIVATE ${DSPI_DIR})
target_link_libraries(dspi_descriptors_z2 PUBLIC tinyusb_z2)
target_compile_options(dspi_descriptors_z2 PRIVATE ${COMMON_FLAGS} -O2 -g -std=c17 -Wall)

add_executable(pizero_z2 ${PLATFORM_SRC} z2/main_z2.c z2/uac1_z2.c)
# z2 first, so its tusb_config.h wins over DSPi's Pico one.
target_include_directories(pizero_z2 PRIVATE ${CMAKE_CURRENT_LIST_DIR}/z2 ${DSPI_DIR})
target_link_libraries(pizero_z2 PRIVATE dspi_descriptors_z2 tinyusb_z2)
pizero_image(pizero_z2)

# USB loader: the SD card's kernel.img once it is set up (sdcard-loader/), loaded at 32 MB. It receives
# any image above over USB into 0x8000 and starts it (tools/pizero_load.py).
add_library(tinyusb_loader STATIC
    ${TUSB_DIR}/tusb.c
    ${TUSB_DIR}/common/tusb_fifo.c
    ${TUSB_DIR}/device/usbd.c
    ${TUSB_DIR}/device/usbd_control.c
    ${TUSB_DIR}/class/vendor/vendor_device.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dcd_dwc2.c
    ${TUSB_DIR}/portable/synopsys/dwc2/dwc2_common.c
)
target_include_directories(tinyusb_loader PUBLIC ${TUSB_DIR} ${CMAKE_CURRENT_LIST_DIR}/loader)
target_link_libraries(tinyusb_loader PUBLIC bcm_peripherals)
target_compile_options(tinyusb_loader PRIVATE ${COMMON_FLAGS} -O2 -g -std=c17 -Wall)

add_executable(pizero_loader ${PLATFORM_SRC} loader/main_loader.c loader/usb_descriptors.c)
target_link_libraries(pizero_loader PRIVATE tinyusb_loader)
pizero_image(pizero_loader pizero_loader.ld)
add_custom_command(TARGET pizero_loader POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/sdcard-loader
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/pizero_loader.img ${CMAKE_CURRENT_BINARY_DIR}/sdcard-loader/kernel.img
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_LIST_DIR}/boot/config_loader.txt ${CMAKE_CURRENT_BINARY_DIR}/sdcard-loader/config.txt
    VERBATIM
)

# UAC2 at High Speed: TinyUSB's audio class, interrupt-driven, on the patched DWC2 driver in tusb_dwc2/.
# Loaded over USB with tools/pizero_load.py (not staged on the SD card).
set(DWC2_DIR ${TUSB_DIR}/portable/synopsys/dwc2)
add_library(tinyusb_uac2 STATIC
    ${TUSB_DIR}/tusb.c
    ${TUSB_DIR}/common/tusb_fifo.c
    ${TUSB_DIR}/device/usbd.c
    ${TUSB_DIR}/device/usbd_control.c
    ${TUSB_DIR}/class/audio/audio_device.c
    ${CMAKE_CURRENT_LIST_DIR}/tusb_dwc2/dcd_dwc2.c
    ${DWC2_DIR}/dwc2_common.c
)
target_include_directories(tinyusb_uac2 PUBLIC ${CMAKE_CURRENT_LIST_DIR}/uac2 ${TUSB_DIR} ${DWC2_DIR})
target_link_libraries(tinyusb_uac2 PUBLIC bcm_peripherals)
target_compile_options(tinyusb_uac2 PRIVATE ${COMMON_FLAGS} -O2 -g -std=c17 -Wall)

add_executable(pizero_uac2 ${PLATFORM_SRC} irq.c uac2/main_uac2.c uac2/usb_descriptors.c)
# uac2 first, so its tusb_config.h wins over the loader's (pizero_image adds loader/ for loader_proto.h).
target_include_directories(pizero_uac2 BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/uac2)
target_link_libraries(pizero_uac2 PRIVATE tinyusb_uac2)
pizero_image(pizero_uac2)

# Z4: DSPi's RP2350 float pipeline on the UAC2 front end. DSPi's sources are compiled unchanged, as
# RP2350 (PICO_RP2350=1), against stand-in Pico SDK headers in pico_shim/. Received audio is decoded
# into the pipeline every millisecond and the output rings are drained by the system timer, standing
# in for the outputs. Loaded over USB (not staged on the SD card). Zero 2 W only, see below.
if(PIZERO_BOARD STREQUAL "zero2")
    set(PICO_EXTRAS_DIR ${CMAKE_CURRENT_LIST_DIR}/../../DSPi/firmware/pico-extras/src/rp2_common)
    set(Z4_DSPI_FILES
        audio_pipeline.c
        usb_audio.c
        dsp_pipeline.c
        crossover.c
        loudness.c
        crossfeed.c
        leveller.c
        psybass.c
        subharm.c
        tube.c
        limiter.c
        upmix.c
        siggen.c
        rta.c
        rta_fft.c
        rta_bass.c
        output_latency.c
        pdm_generator.c
        audio_input.c
        adat_output.c
        spdif_input.c
        input_servo.c
        notify.c
        bg_job.c
        vendor_commands.c
        bulk_params.c
        flash_storage.c
        usb_feedback_controller.c
        input_source_names.c
        lg_sound_sync.c
        dac_hw_mute.c
        uart_control.c
        i2c_control.c
        control_surfaces.c
        control_surfaces_display.c
        control_surfaces_ir.c
        control_surfaces_nouns.c
        usb_descriptors.c
    )
    list(TRANSFORM Z4_DSPI_FILES PREPEND ${DSPI_DIR}/ OUTPUT_VARIABLE Z4_DSPI_SRC)

    add_library(dspi_z4 STATIC ${Z4_DSPI_SRC})
    # build_info.h (REQ_GET_BUILD_INFO), stamped by DSPi's own script on every build, as DSPi does.
    set(Z4_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/z4_generated)
    add_custom_target(dspi_z4_build_info
        COMMAND ${CMAKE_COMMAND} -DREPO_DIR=${DSPI_DIR} -DOUT_FILE=${Z4_GENERATED_DIR}/build_info.h
            -P ${DSPI_DIR}/../../scripts/gen_build_info.cmake
        BYPRODUCTS ${Z4_GENERATED_DIR}/build_info.h
        VERBATIM)
    add_dependencies(dspi_z4 dspi_z4_build_info)
    target_include_directories(dspi_z4 PRIVATE ${Z4_GENERATED_DIR})
    # uac2 first for its tusb_config.h, then the shim, then DSPi and the real pico-extras output headers.
    target_include_directories(dspi_z4 BEFORE PUBLIC ${CMAKE_CURRENT_LIST_DIR}/uac2 ${CMAKE_CURRENT_LIST_DIR}/pico_shim)
    target_include_directories(dspi_z4 PUBLIC
        ${DSPI_DIR}
        ${PICO_EXTRAS_DIR}/pico_audio_spdif_multi/include
        ${PICO_EXTRAS_DIR}/pico_audio_i2s_multi/include
        ${PICO_EXTRAS_DIR}/pico_spdif_rx
    )
    # This also makes vendor requests report WIRE_PLATFORM_RP2350 (1), so an unpatched DSPi Console
    # connects. Temporary, until the Zero gets its own platform ID.
    target_compile_definitions(dspi_z4 PUBLIC PICO_RP2350=1)
    target_link_libraries(dspi_z4 PUBLIC tinyusb_uac2)
    # DSPi builds hosted (so fabsf, fminf and the like are builtins), with -fcommon and gnu C. Its
    # warnings are silenced since its sources cannot change here.
    target_compile_options(dspi_z4 PRIVATE ${COMMON_FLAGS} -fhosted -O2 -g -std=gnu17 -fcommon -w)
    target_link_libraries(dspi_z4 PUBLIC m)
    # Per-file optimisation as in firmware/DSPi/CMakeLists.txt.
    foreach(f usb_audio.c vendor_commands.c audio_pipeline.c crossfeed.c loudness.c psybass.c leveller.c crossover.c
              spdif_input.c i2s_input.c input_servo.c lg_sound_sync.c dac_hw_mute.c siggen.c upmix.c adat_output.c
              adat_input.c loopback.c)
        set_property(SOURCE ${DSPI_DIR}/${f} APPEND PROPERTY COMPILE_OPTIONS -O3)
    endforeach()
    foreach(f dsp_pipeline.c subharm.c tube.c rta_bass.c)
        set_property(SOURCE ${DSPI_DIR}/${f} APPEND PROPERTY COMPILE_OPTIONS -O3 -ffp-contract=off)
    endforeach()
    # usb_audio.c holds the pipeline's globals and usb_sound_card_init(), and also DSPi's UAC1 class
    # driver. Its driver hook is renamed so the image keeps its own vendor driver, its tud_init()
    # becomes a no-op because main_z4.c brings USB up itself, and main_z4.c answers
    # usb_audio_stream_active() from the UAC2 consumer.
    set_property(SOURCE ${DSPI_DIR}/usb_audio.c APPEND PROPERTY COMPILE_DEFINITIONS
        usbd_app_driver_get_cb=dspi_uac1_driver_get_cb
        tud_rhport_init=dspi_z4_tud_rhport_init
        usb_audio_stream_active=dspi_uac1_stream_active
    )

    # vendor_commands.c answers every vendor request on EP0. Its entry point is renamed so main_z4.c's
    # callback can serve the bench requests (0x4F, 0x40) first and pass everything else on.
    set_property(SOURCE ${DSPI_DIR}/vendor_commands.c APPEND PROPERTY COMPILE_DEFINITIONS
        tud_vendor_control_xfer_cb=dspi_vendor_control_xfer_cb
    )

    # usb_descriptors.c is linked only for its serial string buffer and MS OS 2.0 descriptor set, which
    # vendor_commands.c returns. The image enumerates with uac2's descriptors, so DSPi's callbacks are
    # renamed out of the way and dropped by the linker.
    set_property(SOURCE ${DSPI_DIR}/usb_descriptors.c APPEND PROPERTY COMPILE_DEFINITIONS
        tud_descriptor_bos_cb=dspi_tud_descriptor_bos_cb
        tud_descriptor_device_cb=dspi_tud_descriptor_device_cb
        tud_descriptor_configuration_cb=dspi_tud_descriptor_configuration_cb
        tud_descriptor_string_cb=dspi_tud_descriptor_string_cb
    )

    add_executable(pizero_z4 ${PLATFORM_SRC} irq.c mem_ops.c pcm_out.c pcm_rx.c pll_out.c smi_out.c z4/main_z4.c z4/clk_dump.c z4/z4_prof.c z4/z4_mainloop.c z4/z4_globals.c z4/dspi_standins.c z4/spdif_rx_pi.c z4/soft_vcxo_pi.c z4/cpu_meter.c spdif_decode.c pico_shim/pico_shim.c uac2/usb_descriptors.c)
    target_include_directories(pizero_z4 BEFORE PRIVATE ${CMAKE_CURRENT_LIST_DIR}/uac2 ${CMAKE_CURRENT_LIST_DIR}/pico_shim)
    target_link_libraries(pizero_z4 PRIVATE dspi_z4 tinyusb_uac2)
    target_link_options(pizero_z4 PRIVATE -Wl,--wrap=dcd_event_handler)    # USB event accounting, main_z4.c
    set_property(SOURCE mem_ops.c APPEND PROPERTY COMPILE_OPTIONS -fno-builtin -fno-tree-loop-distribute-patterns)
    pizero_image(pizero_z4)
else()
    # ARM1176 cannot assemble usb_audio.c's RP2350 24-bit unpack (inline sbfx/bfi need ARMv6T2).
    message(STATUS "pizero_z4 is built for PIZERO_BOARD=zero2 only")
endif()
