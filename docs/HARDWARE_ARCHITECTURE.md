# Hardware Block Architecture

Hardware interconnect, pinmux, and bus layout diagram for the ARK1668 / Limcet Box P306 head unit. Cross-checked against the vendor reference schematic, board photos, and live hardware bus probes.

## Architecture Flowchart

```mermaid
flowchart TD
    subgraph Band1["01 Vehicle & External I/O"]
        DCDC["B+ / GND → DC-DC Converter<br/>+5V / +3V3 / +9V rails"]
        CANWire["CAN H / CAN L<br/>Vehicle CAN Bus Wiring"]
        VehicleSignals["ACC / ILL / SWC<br/>Ignition, Dimming, Steering Controls"]
        ISOAntenna["ISO Antenna Socket<br/>AM/FM Radio Input"]
        SpeakerWires["Speaker Wires<br/>FR± / FL± / RR± / RL±"]
        CamPower["CAM PWR<br/>Reversing Camera Power Feed"]
    end

    subgraph Band2["02 On-Board Support ICs"]
        MCU["MCU — STM32F105<br/>Vehicle I/O Hub (/dev/ttyHS0)"]
        CANTrx["CAN Transceiver<br/>NXP TJA1042"]
        Tuner["AM/FM Tuner<br/>I2C Control"]
        DSP["DSP / Tone Processor<br/>Audio EQ / Channel Mix"]
        VoiceProc["Voice Processor (Schematic Only)<br/>*Not populated on this board*"]
    end

    subgraph Band3["03 Application Processor"]
        ARKBrain["ARK1668 / ARK1680 SoC<br/>ARM Cortex-A5 · LCDC · Vivante GPU · Hantro hx170dec<br/>NAND/SDRAM Ctrl · Internal Audio ADC/DAC"]
    end

    subgraph Band4["04 Attached Peripheral ICs"]
        NAND["NAND Flash<br/>128 MiB SLC (TC58BVG0S3HTA00)"]
        SDRAM["SDRAM<br/>Nanya NT5CC128M16IP (DDR3)"]
        RN6752["RN6752 Video Decoder<br/>AHD/CVBS Camera @ I2C 0x2c"]
        ARK7116["ARK7116 Video Decoder<br/>*Reference design only — not populated*"]
        USBHost["USB Host / OTG<br/>USB0 (Ext/Gadget) / USB1 (WiFi)"]
        WiFi["RTL8811CU / RTL8821CU<br/>WiFi Module (via USB1 Host)"]
        BT["BT Module (FSC-BT8251)<br/>ttyHS1 @ 1.5M, GPIO91 Power"]
    end

    subgraph Band5["05 Output & Analog Endpoints"]
        TFT["TFT LCD Panel<br/>800×480 RGB888 / LVDS + Resistive Touch"]
        PowerAmp["Power Amp / Sound IC<br/>Rohm BD37033FV (I2C2 @ 0x40)"]
        Mic["Microphone Input<br/>SoC Internal sdadc / I2S-ADC"]
        Camera["Reversing Camera<br/>CVBS Analog Video Feed"]
    end

    %% Power Distribution
    DCDC ==>|Power Rails| ARKBrain
    DCDC ==>|Power Rails| MCU
    CamPower -.->|Power Feed| DCDC

    %% Vehicle & Radio to Support ICs
    CANWire <===>|CAN Bus| CANTrx
    CANTrx <===>|Rx/Tx Serial| MCU
    VehicleSignals ===>|Digital/Analog In| MCU
    ISOAntenna --->|RF Analog| Tuner
    Tuner -.->|I2C Control| MCU

    %% MCU to SoC
    MCU <===>|UART /dev/ttyHS0 115200 8N1| ARKBrain
    MCU -.->|I2C Control| DSP
    DSP --->|Analog Audio| PowerAmp
    PowerAmp ===>|Amplified Audio| SpeakerWires

    %% Voice Processor (Dormant) vs Real Mic Path
    VoiceProc -.->|Ref Schematic Only| Mic
    ARKBrain <--->|Real Path: SoC sdadc| Mic

    %% Memory & Storage
    ARKBrain <===>|Parallel NAND Bus| NAND
    ARKBrain <===>|DDR3 Bus| SDRAM

    %% Camera Video Decoder
    Camera --->|CVBS Analog Video| RN6752
    RN6752 ===>|ITU-656 Video Bus| ARKBrain
    ARKBrain <--->|I2C Control 0x2c| RN6752
    ARKBrain -.->|Ref Design Only| ARK7116

    %% Wireless & USB
    ARKBrain <===>|USB Bus (usb1)| WiFi
    ARKBrain <===>|UART /dev/ttyHS1 (1.5M)| BT
    ARKBrain <===>|USB Bus (usb0)| USBHost
    BT --->|Analog Mic / AEC| Mic

    %% Display & Audio Outputs
    ARKBrain ===>|RGB888 / LVDS Video| TFT
    TFT ===>|Resistive Touch (ADC 0xe4500000)| ARKBrain
    ARKBrain --->|I2S1 / Internal DAC| DSP

    %% Styling
    classDef core fill:#d4edda,stroke:#28a745,color:#155724
    classDef storage fill:#d1ecf1,stroke:#17a2b8,color:#0c5460
    classDef dormant fill:#f8f9fa,stroke:#6c757d,color:#6c757d,stroke-dasharray: 4 4
    classDef defaultNode fill:#fff3cd,stroke:#e0a800,color:#856404

    class ARKBrain,MCU core
    class NAND,SDRAM storage
    class VoiceProc,ARK7116 dormant
```

---

## Architectural Insights & Hardware Realities

### 1. Discrepancies with Vendor Reference Schematic
- **Camera Decoder:** The vendor reference schematic specifies an `ARK7116` decoder. This board actually populates an **RN6752** decoder at 7-bit I2C address `0x2c` (`dvr_rn6752@2c`).
- **Voice Processor:** The reference schematic indicates a dedicated SPI/I2S voice processor IC for acoustic echo cancellation (AEC). On this board, there is **no SPI bus** node or external voice IC. Echo cancellation is handled in software (`HFP_NREC=3` in `blueware`), and audio input routes through the SoC's internal ADC (`sdadc` / `i2s-adc`).
- **WiFi Module:** The reference schematic does not depict WiFi. On this board, a Realtek **RTL8811CU / RTL8821CU** module is connected internally over USB (`usb1`).

### 2. Microcontroller & Vehicle Bus
- **MCU Identification:** An **STM32F105RBT6** (ARM Cortex-M3) is visible on the board and communicates with the ARK1668 over `/dev/ttyHS0` (`0xe4f00000`, 115200 8N1).
- **CAN Bus Integration:** Steering wheel controls, reverse signal detection, ignition (ACC), and vehicle telemetry are decoded by the MCU directly from the vehicle's CAN bus via an **NXP TJA1042** transceiver and passed to the SoC via structured UART packets.

### 3. Display & Touchscreen Subsystem
- **Direct Touch Routing:** The 4-wire resistive touchscreen layer routes straight to the ARK1668 SoC's dedicated resistive touchscreen controller (`ark1680_ts.ko` @ `0xe4500000`), completely bypassing the MCU.
- **Display Output:** The video stream is driven via 24-bit RGB888 through an interposer board (`DC_FUJITSU_CON96P_REV_002`) to the factory LCD panel.
