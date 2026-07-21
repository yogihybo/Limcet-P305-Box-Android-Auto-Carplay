# Handoff: Implement VDE Config Ioctls in mock `ark_display` driver

You are an AI coding assistant working on a Linux VM to build and test the reconstructed kernel for a Toyota Prado infotainment unit (SoC: Ark1668/1680). 

Your task is to modify the mock display driver `hardware/ark_display.c` to add in-memory support for the display's VDE (Video Display Engine) brightness, contrast, saturation, and hue ioctl queries. This will resolve the boot-time warnings (`ark_display: unhandled ioctl cmd=0xc004a001`) generated when the stock userspace application queries display settings.

---

### Context & Findings

1. **Target File:** `hardware/ark_display.c`
2. **Missing Ioctls:**
   - **`ARKDISP_GET_VDE_CFG` (`0xc004a001`)**: `_IOWR(0xa0, 1, unsigned long)`
   - **`ARKDISP_SET_VDE_CFG` (`0x4004a002`)**: `_IOW(0xa0, 2, unsigned long)`
   *Note: As with `ARKDISP_GET_SCREEN_INFO`, these macros must use `unsigned long` as the size parameter to match the stock userspace library's ioctl command-number encoding convention, regardless of the actual structure payload size.*

3. **Data Structure:**
   Define the configuration structure matching the reference BSP header:
   ```c
   struct ark_disp_vde_cfg_arg {
       __u32 layer_id;
       __u32 hue;
       __u32 saturation;
       __u32 brightness;
       __u32 contrast;
   };
   ```

4. **Tracking Layer Settings:**
   The SoC display controller handles 5 layers (3 OSD layers and 2 Video layers). We should track their VDE settings in a static array initialized with reasonable defaults:
   - Hue: `0`
   - Saturation, Brightness, Contrast: `128` (0x80)

---

### Implementation Instructions

1. **Update `hardware/ark_display.c`**:
   - Add the macro definitions:
     ```c
     #define ARKDISP_GET_VDE_CFG    _IOWR(ARK_DISPLAY_IOC_MAGIC, 1, unsigned long)
     #define ARKDISP_SET_VDE_CFG    _IOW(ARK_DISPLAY_IOC_MAGIC, 2, unsigned long)
     #define ARK_DISPLAY_LAYER_NUM  5
     ```
   - Declare `struct ark_disp_vde_cfg_arg`.
   - Implement a static array to track the states:
     ```c
     static struct ark_disp_vde_cfg_arg ark_display_layers[ARK_DISPLAY_LAYER_NUM] = {
         { .layer_id = 0, .hue = 0, .saturation = 128, .brightness = 128, .contrast = 128 },
         { .layer_id = 1, .hue = 0, .saturation = 128, .brightness = 128, .contrast = 128 },
         { .layer_id = 2, .hue = 0, .saturation = 128, .brightness = 128, .contrast = 128 },
         { .layer_id = 3, .hue = 0, .saturation = 128, .brightness = 128, .contrast = 128 },
         { .layer_id = 4, .hue = 0, .saturation = 128, .brightness = 128, .contrast = 128 },
     };
     ```
   - Update `ark_display_ioctl` to handle `ARKDISP_GET_VDE_CFG` and `ARKDISP_SET_VDE_CFG`:
     - **GET**: Read the `layer_id` from the input argument, validate that it is `< 5`, copy the values from the matching array index, and use `copy_to_user` to return the updated structure.
     - **SET**: Validate the `layer_id`, write the updated hue, saturation, brightness, and contrast values into the static array, and log the action.

---

### Verification Instructions

1. **Rebuild the Kernel**:
   Run the kernel compilation script or command in the VM (ensuring `CONFIG_ARK_DISPLAY=y` is active) to rebuild the `zImage` containing the updated driver.
2. **Flash & Test**:
   - Flash the built kernel to the Prado hardware.
   - Boot the unit and monitor the boot console output.
   - Confirm that the `ark_display: unhandled ioctl cmd=0xc004a001` logs are gone.
   - Confirm that the driver prints messages like `ark_display: ARKDISP_GET_VDE_CFG -> layer_id=...` when the settings app initializes or adjustments are made in the UI.
