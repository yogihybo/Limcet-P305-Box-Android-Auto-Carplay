# Trigger BD37033 initialization live on silicon
gdb_port disabled
tcl_port disabled
telnet_port disabled

init

echo "======================================================="
echo " PROVING BD37033 TRIGGER ON LIVE STM32F105 SILICON"
echo "======================================================="

# Reset target to fresh boot
reset halt
mww 0xE0042004 0x00000307
resume
sleep 500
halt
mww 0xE0042004 0x00000307

echo "--- Initial State at 0.5s ---"
echo "Audio Step (*0x2000102D): [format 0x%02X [mrb 0x2000102D]]"
echo "Audio Timer (*0x2000007E): [mrh 0x2000007E]"
echo "I2C1->CR1: [format 0x%08X [mrw 0x40005400]]"
echo "GPIOB->CRL: [format 0x%08X [mrw 0x40010C00]]"

# Fast-forward the settling timer to 3999 (0x0F9F) so next tick triggers >= 4000
echo "\n--- Fast-forwarding Audio Timer to 3999 (0x0F9F) ---"
mwh 0x2000007E 0x0F9F

# Keep power manager awake by resetting power timer
mwh 0x20000052 0x0000

# Let firmware run for 200ms to process Queue 3 and execute BD37033 init
resume
sleep 200
halt
mww 0xE0042004 0x00000307

echo "\n--- State After Settling Trigger (t = 0.7s) ---"
echo "Audio Step (*0x2000102D): [format 0x%02X [mrb 0x2000102D]]"
echo "Audio Timer (*0x2000007E): [mrh 0x2000007E]"
echo "I2C Error Count (*0x20000094): [format 0x%02X [mrb 0x20000094]]"
echo "I2C1->CR1: [format 0x%08X [mrw 0x40005400]]"
echo "I2C1->CR2: [format 0x%08X [mrw 0x40005404]]"
echo "I2C1->SR1: [format 0x%08X [mrw 0x40005414]]"
echo "I2C1->SR2: [format 0x%08X [mrw 0x40005418]]"
echo "GPIOB->CRL (PB6/PB7 Alternate Function): [format 0x%08X [mrw 0x40010C00]]"

# Check volume and fader RAM block at 0x20001016
echo "\nAudio Config Block (*0x20001016..0x20001025):"
mdw 0x20001014 4

echo "======================================================="
resume
exit
