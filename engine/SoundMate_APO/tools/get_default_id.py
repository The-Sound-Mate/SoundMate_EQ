import comtypes
from comtypes import CLSCTX_ALL
import ctypes

# MMDeviceAPI constants
CLSID_MMDeviceEnumerator = "{BCDE0395-E52F-467C-8E3D-C4579291692E}"
IID_IMMDeviceEnumerator = "{A95664D2-9614-4F35-A746-DE8DB63617E6}"

def get_default_device_id():
    try:
        # Initialize COM
        ctypes.oledll.ole32.CoInitialize(None)
        
        # Create Enumerator
        enumerator = comtypes.CoCreateInstance(
            CLSID_MMDeviceEnumerator,
            interface=comtypes.gen.MMDeviceAPILib.IMMDeviceEnumerator,
            clsctx=CLSCTX_ALL
        )
        
        # Get Default Endpoint (eRender, eConsole)
        device = enumerator.GetDefaultAudioEndpoint(0, 0)
        
        # Get ID
        device_id = device.GetId()
        print(f"DEFAULT_DEVICE_ID: {device_id}")
        
        # Get Name
        props = device.OpenPropertyStore(0) # STGM_READ
        from comtypes.gen.MMDeviceAPILib import _tagpropertykey
        PKEY_Device_FriendlyName = _tagpropertykey()
        PKEY_Device_FriendlyName.fmtid = comtypes.GUID("{a45c254e-df1c-4efd-8020-67d146a850e0}")
        PKEY_Device_FriendlyName.pid = 2
        
        var = props.GetValue(PKEY_Device_FriendlyName)
        print(f"DEFAULT_DEVICE_NAME: {var.pwszVal}")
        
    except Exception as e:
        print(f"Error: {e}")
    finally:
        ctypes.oledll.ole32.CoUninitialize()

if __name__ == "__main__":
    get_default_device_id()
