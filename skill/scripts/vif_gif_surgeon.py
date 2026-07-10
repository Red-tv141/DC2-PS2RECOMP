import struct
import sys
import argparse

# Usage: python vif_gif_surgeon.py <path_to_memory_dump_bin> <start_offset_hex>
# Example: python vif_gif_surgeon.py ram_dump.bin 0x1A20000

def decode_vifcode(cmd):
    """Basic VIF code decoding for diagnostics"""
    cmd_type = (cmd >> 24) & 0x7F
    num = (cmd >> 16) & 0xFF
    imm = cmd & 0xFFFF
    
    types = {
        0x00: "NOP",
        0x01: "STCYCL",
        0x02: "OFFSET",
        0x03: "BASE",
        0x04: "ITOP",
        0x05: "STMOD",
        0x06: "MSKPATH3",
        0x07: "MARK",
        0x10: "FLUSHE",
        0x11: "FLUSH",
        0x13: "FLUSHA",
        0x14: "MSCAL",
        0x15: "MSCNT",
        0x17: "MSCALF",
        0x20: "STMASK",
        0x30: "STROW",
        0x31: "STCOL",
        0x4A: "MPG",
        0x50: "DIRECT",
        0x51: "DIRECTHL"
    }
    
    # UNPACK commands are >= 0x60
    if cmd_type >= 0x60:
        vn = (cmd_type >> 2) & 0x3
        vl = cmd_type & 0x3
        return f"UNPACK (vn={vn}, vl={vl}) num={num} imm=0x{imm:04X}"
        
    return f"{types.get(cmd_type, 'UNKNOWN(0x{:02X})'.format(cmd_type))} num={num} imm=0x{imm:04X}"

def analyze_dma_packet(data, offset):
    print(f"--- VIF/GIF Surgeon: Analyzing DMA Packet at offset 0x{offset:08X} ---")
    if len(data) < offset + 16:
        print("Error: Offset out of bounds or packet too small.")
        return

    # A standard DMA Class 2 (Source Chain) tag is 128 bits (16 bytes)
    # 64 bits DMA Tag | 64 bits VIF code (usually 2x 32bit VIF codes)
    
    # Read the first 16 bytes
    qword = data[offset:offset+16]
    
    # Unpack DMA Tag (lower 64 bits)
    dma_lo, dma_hi = struct.unpack("<II", qword[0:8])
    qwc = dma_lo & 0xFFFF
    pce = (dma_lo >> 26) & 0x3
    id_val = (dma_lo >> 28) & 0x7
    irq = (dma_lo >> 31) & 0x1
    addr = dma_hi
    
    dma_ids = {0: "REFE", 1: "CNT", 2: "NEXT", 3: "REF", 4: "REFS", 5: "CALL", 6: "RET", 7: "END"}
    id_str = dma_ids.get(id_val, f"UNKNOWN({id_val})")
    
    print(f"DMA TAG:")
    print(f"  QWC (QuadWord Count): {qwc}")
    print(f"  ID: {id_str}")
    print(f"  ADDR: 0x{addr:08X}")
    print(f"  IRQ: {irq}, PCE: {pce}")
    
    if qwc == 0 and id_val not in [0, 5, 6, 7]: # REFE, CALL, RET, END can have QWC 0
         print("  [!] WARNING: Suspicious DMA Tag with QWC=0 and ID that normally expects data.")

    # Unpack VIF codes (upper 64 bits)
    vif0, vif1 = struct.unpack("<II", qword[8:16])
    print(f"\nVIF CODES (In DMA Tag):")
    print(f"  VIF0: 0x{vif0:08X} -> {decode_vifcode(vif0)}")
    print(f"  VIF1: 0x{vif1:08X} -> {decode_vifcode(vif1)}")
    
    # Check for DIRECT command potential issues
    cmd_type_vif1 = (vif1 >> 24) & 0x7F
    if cmd_type_vif1 in [0x50, 0x51]: # DIRECT or DIRECTHL
        direct_qwc = vif1 & 0xFFFF
        print(f"\n[!] DIRECT COMMAND DETECTED")
        print(f"  DIRECT expects {direct_qwc} QWORDs of payload to follow to the GIF.")
        if direct_qwc != qwc:
             print(f"  [CRITICAL ERROR ALARM]: DMA Tag QWC ({qwc}) DOES NOT MATCH VIF DIRECT QWC ({direct_qwc}). This WILL crash the GS/VU pipeline!")
             
    print("\n--- End of Packet Header Diagnosis ---")


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python vif_gif_surgeon.py <path_to_memory_dump_bin> <start_offset_hex>")
        sys.exit(1)
        
    filename = sys.argv[1]
    offset = int(sys.argv[2], 16)
    
    try:
        with open(filename, "rb") as f:
            data = f.read()
        analyze_dma_packet(data, offset)
    except FileNotFoundError:
        print(f"Could not open file: {filename}")
