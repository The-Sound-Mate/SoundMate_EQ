import struct
import time
import ctypes
from ctypes import wintypes

# SoundMate Shared Memory Structure
SHM_NAME = "Global\\SoundMate_APO_SHM"
SHM_SIZE = 256
FILE_MAP_ALL_ACCESS = 0xF001F

kernel32 = ctypes.windll.kernel32

def run_test():
    print("===============================================")
    print("   SoundMate Real-time Engine Tester (API)     ")
    print("===============================================")
    
    # Try to OPEN existing SHM created by the Engine
    hMap = kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, SHM_NAME)

    if not hMap:
        print(f"[Tester] Failed to open SHM ({SHM_NAME}). Error: {kernel32.GetLastError()}")
        print("[Note] The Engine might not be running or failed to create SHM.")
        return

    print(f"[Tester] Successfully connected to Engine's SHM: {SHM_NAME}")

    pBuf = kernel32.MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, SHM_SIZE)
    if not pBuf:
        print(f"[Tester] Failed to map view. Error: {kernel32.GetLastError()}")
        kernel32.CloseHandle(hMap)
        return

    magic = 0x534D5445
    version = 1
    update_counter = 0

    # Default 10 bands (must match SoundMate_Shared.h)
    bands = []
    for i in range(10):
        if i == 0: bands.extend([1, 60.0, -30.0, 1.0])
        elif i == 1: bands.extend([1, 8000.0, -30.0, 1.0])
        else: bands.extend([0, 1000.0, 0.0, 1.0])

    def update_shm(g, u_count):
        # I I f I (magic, version, masterGain, bandCount)
        # 10 * (I f f f) (bands)
        # Q (updateCounter)
        data = struct.pack("I I f I " + ("I f f f " * 10) + "Q", 
                           magic, version, g, 10, *bands, u_count)
        ctypes.memmove(pBuf, data, len(data))
        print(f" -> [Update] MasterGain: {g:.2f}, Counter: {u_count}")

    print("\n[Control] Enter Master Gain (e.g., 0.1, 1.0, 0.0) or 'q' to quit.")
    
    try:
        while True:
            user_input = input("New Master Gain: ").strip()
            if user_input.lower() == 'q': break
            try:
                new_gain = float(user_input)
                update_counter = int(time.time()) # Use timestamp as counter
                update_shm(new_gain, update_counter)
            except ValueError:
                print("Invalid input.")
    finally:
        kernel32.UnmapViewOfFile(pBuf)
        kernel32.CloseHandle(hMap)
        print("[Tester] Closed.")

if __name__ == "__main__":
    run_test()
