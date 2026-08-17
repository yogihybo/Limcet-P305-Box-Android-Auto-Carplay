# MsnCoreApp Process Architecture

How the head unit's Qt/QWS application, its `dlopen`'d feature plugins, external protocol daemons, and shared hardware-abstraction libraries fit together on the ARK1668 / Limcet P306 platform.

## Process & IPC Flowchart

```mermaid
flowchart TD
    subgraph Band1["01 MsnCoreApp Main Process (Qt 4.7.4 QWS — Single PID)"]
        MainApp["MsnCoreApp<br/>/usr/bin/MsnCoreApp"]
        Commons["libMsnCommons.so<br/>MsnIniConfig · ArkUtils · ArkDbus"]
        pLauncher["libLauncher-Box.so<br/>(Plugin 10 — Main UI / Launcher)"]
        pSetting["libSetting.so<br/>(Plugin 8 — Settings / Factory Menu)"]
        pCarAuto["libMsnCarAuto.so<br/>(Plugin 13 — Android Auto UI)"]
        pCarPlay["libMsnCarPlay.so<br/>(Plugin 4 — Apple CarPlay UI)"]
        pReversing["libCarReversing.so<br/>(Plugin 12 — Reversing Camera Overlay)"]
        pBT["libBlueTooth.so<br/>(Plugin 3 — Bluetooth Pairing/HFP UI)"]
        pCanBus["libCanBus.so<br/>(Plugin 400 — CAN Signal Bridge)"]
        pMcu["libMcuCenter.so<br/>(Plugin 401 — MCU Adapter BoxP300)"]
        pSound["libMsnSound.so<br/>(Plugin 403 — Sound / ALSA Mixer)"]
    end

    subgraph Band2["02 IPC & Transport"]
        DBusDaemon["dbus-daemon<br/>Session Bus & Service Activation"]
        UNIXSock["AF_UNIX Domain Sockets<br/>ArkUtils::open_local_socket"]
        UARTLinks["UART Serial Links<br/>ttyHS0 (MCU) · ttyHS1 (BT)"]
    end

    subgraph Band3["03 Protocol Daemons (External Processes)"]
        SinkDaemon["sink<br/>com.arkmicro.auto (Android Auto Engine)"]
        CarPlayDaemon["carplay<br/>com.arkmicro.carplay (CarPlay Engine)"]
        BluewareDaemon["blueware<br/>HFP / A2DP Stack & AT Interface"]
        HostapdDaemon["hostapd + udhcpd<br/>carplay_wifi AP Provider"]
    end

    subgraph Band4["04 Shared HAL & Middleware"]
        libArkCmn["libarkcmn.so<br/>arkapi_* ioctl wrapper"]
        libGAL["libGAL.so<br/>Vivante GC GPU DirectFB Backend"]
        libMFC["libmfc.so<br/>Hantro DWL H.264 Video Decode"]
        libQExt["libqextserialport.so<br/>Serial Port Abstraction"]
    end

    subgraph Band5["05 Kernel Drivers & Device Nodes"]
        DevFB["/dev/fb0 - /dev/fb4<br/>ark1668_lcdfb (OSD & Video Layers)"]
        DevHX170["/dev/hx170dec<br/>Hantro G1 Video Decoder"]
        DevDVR["/dev/dvr<br/>ITU-656 Camera Capture"]
        DevTTYHS0["/dev/ttyHS0<br/>ark-hsuart (MCU Link)"]
        DevTTYHS1["/dev/ttyHS1<br/>ark-hsuart (Bluetooth Module)"]
        DevALSA["ALSA / I2S<br/>i2s-dac / i2s-adc ↔ BD37033"]
        DevTouch["/dev/input/event0<br/>ark1680_ts Resistive Touch"]
        DevWlan["wlan0<br/>RTL8811CU / RTL8821CU USB WiFi"]
    end

    %% In-process dynamic loading
    MainApp --- Commons
    MainApp --- pLauncher
    MainApp --- pSetting
    MainApp --- pCarAuto
    MainApp --- pCarPlay
    MainApp --- pReversing
    MainApp --- pBT
    MainApp --- pCanBus
    MainApp --- pMcu
    MainApp --- pSound

    %% D-Bus Service Activation
    pCarAuto -->|D-Bus StartService| DBusDaemon
    pCarPlay -->|D-Bus StartService| DBusDaemon
    DBusDaemon -->|Spawns| SinkDaemon
    DBusDaemon -->|Spawns| CarPlayDaemon

    %% Local Domain Sockets
    pCarAuto <===>|AF_UNIX Socket (Control)| UNIXSock
    UNIXSock <===> SinkDaemon
    pCarPlay <===>|AF_UNIX Socket (Control)| UNIXSock
    UNIXSock <===> CarPlayDaemon

    %% Video Decode Pipeline
    SinkDaemon ===>|Decode H.264| libMFC
    libMFC ===>|ioctl| DevHX170
    SinkDaemon ===>|Render Video Plane| DevFB
    CarPlayDaemon ===>|Render Video Plane| DevFB

    %% GUI & Display Rendering
    MainApp -.->|DirectFB / fbdev| libGAL
    libGAL -.->|2D/3D GPU Compose| DevFB
    pSetting --->|arkapi_* ioctl| libArkCmn
    libArkCmn ---> DevFB

    %% Camera Overlay
    pReversing --->|Video Switch & Capture| libArkCmn
    libArkCmn ---> DevDVR

    %% MCU & Vehicle I/O
    pMcu ===>|Serial Packets| libQExt
    libQExt ===> UARTLinks
    UARTLinks ===> DevTTYHS0

    %% Bluetooth & WiFi Orchestration
    pBT ===>|AT Commands| BluewareDaemon
    pCarAuto ===>|Trigger BT Pairing| BluewareDaemon
    BluewareDaemon ===> DevTTYHS1
    pCarAuto ===>|Start AP| HostapdDaemon
    HostapdDaemon ===> DevWlan

    %% Audio Subsystem
    pSound --->|ALSA Mixer / EQ| DevALSA
    SinkDaemon --->|Media Audio PCM| DevALSA
    BluewareDaemon --->|Call / HFP Audio| DevALSA

    %% Input Events
    DevTouch ===>|Touch Coordinates| pLauncher

    %% Styling
    classDef core fill:#d4edda,stroke:#28a745,color:#155724
    classDef daemon fill:#fff3cd,stroke:#e0a800,color:#856404
    classDef hal fill:#d1ecf1,stroke:#17a2b8,color:#0c5460
    classDef driver fill:#f8d7da,stroke:#dc3545,color:#721c24

    class MainApp,Commons core
    class SinkDaemon,CarPlayDaemon,BluewareDaemon,HostapdDaemon daemon
    class libArkCmn,libGAL,libMFC,libQExt hal
    class DevFB,DevHX170,DevDVR,DevTTYHS0,DevTTYHS1,DevALSA,DevTouch,DevWlan driver
```

---

## Architectural Insights

### 1. Unified Single-Process Plugin Architecture
- All UI surfaces (`libLauncher-Box.so`, `libSetting.so`, `libMsnCarAuto.so`, `libMsnCarPlay.so`) run inside **`MsnCoreApp`** under a single PID via `dlopen()`.
- They share one Qt event loop and render to a shared surface. Switching views is an in-process widget swap.

### 2. D-Bus Service Activation & Socket Protocol
- Heavy protocol work (Android Auto, Apple CarPlay) is isolated into separate helper daemons (`sink`, `carplay`).
- `MsnCoreApp` triggers their startup via `dbus-daemon` service activation (`com.arkmicro.auto`, `com.arkmicro.carplay`).
- Once active, high-speed binary control passes over an `AF_UNIX` local socket (`ArkUtils::open_local_socket`).

### 3. Hardware Video Decode & Plane Direct Blitting
- Android Auto / CarPlay H.264 video frames decoded by the external daemon are processed by the SoC's hardware **Hantro G1** IP block via `libmfc.so` (`/dev/hx170dec`).
- Video frames are blitted directly onto hardware overlay planes (`VIDEO_LAYER1`/`VIDEO_LAYER2`) in `ark1668_lcdfb` (`/dev/fb1`/`/dev/fb2`), avoiding Qt rendering overhead.

### 4. Direct Serial MCU Communication
- Vehicle signals (CAN bus keys, reverse state, ignition) bypass D-Bus and are received directly by `libMcuCenter.so` over `/dev/ttyHS0` at 115200 baud.
- Bluetooth communication (`blueware`) uses standard AT commands over high-speed serial `/dev/ttyHS1` at 1.5 Mbps.
