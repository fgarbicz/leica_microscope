// Interactive WinUSB probe used to reverse-engineer the Leica USB3 camera protocol.
using System;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

public class UsbProbe : IDisposable {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFile(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
    [DllImport("winusb.dll", SetLastError = true)] static extern bool WinUsb_Initialize(SafeFileHandle h, out IntPtr ih);
    [DllImport("winusb.dll", SetLastError = true)] static extern bool WinUsb_Free(IntPtr ih);
    [DllImport("winusb.dll", SetLastError = true)] static extern bool WinUsb_WritePipe(IntPtr ih, byte pipe, byte[] buf, int len, out int done, IntPtr ov);
    [DllImport("winusb.dll", SetLastError = true)] static extern bool WinUsb_ReadPipe(IntPtr ih, byte pipe, byte[] buf, int len, out int done, IntPtr ov);
    [DllImport("winusb.dll", SetLastError = true)] static extern bool WinUsb_SetPipePolicy(IntPtr ih, byte pipe, uint policy, int len, ref uint val);
    [DllImport("winusb.dll", SetLastError = true)] static extern bool WinUsb_ResetPipe(IntPtr ih, byte pipe);
    [DllImport("winusb.dll", SetLastError = true)] static extern bool WinUsb_AbortPipe(IntPtr ih, byte pipe);
    [DllImport("winusb.dll", SetLastError = true)] static extern bool WinUsb_SetCurrentAlternateSetting(IntPtr ih, byte alt);
    [DllImport("winusb.dll", SetLastError = true)] static extern bool WinUsb_GetDescriptor(IntPtr ih, byte type, byte idx, ushort lang, byte[] buf, int len, out int done);
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct Setup { public byte RequestType, Request; public ushort Value, Index, Length; }
    [DllImport("winusb.dll", SetLastError = true)] static extern bool WinUsb_ControlTransfer(IntPtr ih, Setup sp, byte[] buf, int len, out int done, IntPtr ov);

    SafeFileHandle file;
    IntPtr h;
    public int LastError;

    public UsbProbe(string path) {
        file = CreateFile(path, 0xC0000000, 3, IntPtr.Zero, 3, 0x40000080, IntPtr.Zero);
        if (file.IsInvalid) throw new Exception("open failed " + Marshal.GetLastWin32Error());
        if (!WinUsb_Initialize(file, out h)) throw new Exception("WinUsb_Initialize failed " + Marshal.GetLastWin32Error());
        foreach (byte p in new byte[] { 0x01, 0x81, 0x82, 0x83 }) SetTimeout(p, 1000);
    }
    public void SetTimeout(byte pipe, uint ms) { WinUsb_SetPipePolicy(h, pipe, 3 /*PIPE_TRANSFER_TIMEOUT*/, 4, ref ms); }
    public bool Alt(byte a) { return WinUsb_SetCurrentAlternateSetting(h, a); }
    public void Reset(byte p) { WinUsb_ResetPipe(h, p); }
    public void Abort(byte p) { WinUsb_AbortPipe(h, p); }

    public int Write(byte pipe, byte[] data) {
        int done; if (!WinUsb_WritePipe(h, pipe, data, data.Length, out done, IntPtr.Zero)) { LastError = Marshal.GetLastWin32Error(); return -1; }
        return done;
    }
    public byte[] Read(byte pipe, int len) {
        var b = new byte[len]; int done;
        if (!WinUsb_ReadPipe(h, pipe, b, len, out done, IntPtr.Zero)) { LastError = Marshal.GetLastWin32Error(); return null; }
        Array.Resize(ref b, done); return b;
    }
    public byte[] Control(byte type, byte req, ushort val, ushort idx, int len, byte[] outData = null) {
        var sp = new Setup { RequestType = type, Request = req, Value = val, Index = idx, Length = (ushort)(outData != null ? outData.Length : len) };
        var b = outData ?? new byte[len]; int done;
        if (!WinUsb_ControlTransfer(h, sp, b, b.Length, out done, IntPtr.Zero)) { LastError = Marshal.GetLastWin32Error(); return null; }
        Array.Resize(ref b, done); return b;
    }
    public byte[] Descriptor(byte type, byte idx, ushort lang, int len) {
        var b = new byte[len]; int done;
        if (!WinUsb_GetDescriptor(h, type, idx, lang, b, len, out done)) { LastError = Marshal.GetLastWin32Error(); return null; }
        Array.Resize(ref b, done); return b;
    }

    // ---- GenCP / USB3 Vision control channel ----
    ushort reqId = 1;
    public byte[] GenCpRead(ulong addr, ushort count, out ushort status) {
        var cmd = new byte[12 + 12];
        BitConverter.GetBytes(0x43564755u).CopyTo(cmd, 0); // "U3VC"
        BitConverter.GetBytes((ushort)0x4000).CopyTo(cmd, 4); // request ack
        BitConverter.GetBytes((ushort)0x0800).CopyTo(cmd, 6); // READMEM_CMD
        BitConverter.GetBytes((ushort)12).CopyTo(cmd, 8);
        BitConverter.GetBytes(reqId).CopyTo(cmd, 10);
        BitConverter.GetBytes(addr).CopyTo(cmd, 12);
        BitConverter.GetBytes((ushort)0).CopyTo(cmd, 20);
        BitConverter.GetBytes(count).CopyTo(cmd, 22);
        status = 0xFFFF;
        reqId++;
        if (Write(0x01, cmd) < 0) return null;
        var r = Read(0x81, 1024 + 12 + count);
        if (r == null || r.Length < 12) return r;
        status = BitConverter.ToUInt16(r, 4);
        var payload = new byte[r.Length - 12];
        Array.Copy(r, 12, payload, 0, payload.Length);
        return payload;
    }
    public ushort GenCpWrite(ulong addr, byte[] data) {
        var cmd = new byte[12 + 8 + data.Length];
        BitConverter.GetBytes(0x43564755u).CopyTo(cmd, 0);
        BitConverter.GetBytes((ushort)0x4000).CopyTo(cmd, 4);
        BitConverter.GetBytes((ushort)0x0802).CopyTo(cmd, 6); // WRITEMEM_CMD
        BitConverter.GetBytes((ushort)(8 + data.Length)).CopyTo(cmd, 8);
        BitConverter.GetBytes(reqId++).CopyTo(cmd, 10);
        BitConverter.GetBytes(addr).CopyTo(cmd, 12);
        data.CopyTo(cmd, 20);
        if (Write(0x01, cmd) < 0) return 0xFFFF;
        var r = Read(0x81, 64);
        if (r == null || r.Length < 12) return 0xFFFE;
        return BitConverter.ToUInt16(r, 4);
    }

    public static string Hex(byte[] b, int max = 256) {
        if (b == null) return "<null>";
        var sb = new StringBuilder();
        for (int i = 0; i < Math.Min(b.Length, max); i++) { sb.Append(b[i].ToString("X2")); sb.Append(i % 16 == 15 ? "\n" : " "); }
        if (b.Length > max) sb.Append("... (" + b.Length + " bytes)");
        return sb.ToString();
    }
    public static string Ascii(byte[] b) {
        if (b == null) return "";
        var sb = new StringBuilder();
        foreach (var c in b) sb.Append(c >= 32 && c < 127 ? (char)c : '.');
        return sb.ToString();
    }
    public void Dispose() { if (h != IntPtr.Zero) WinUsb_Free(h); h = IntPtr.Zero; file.Dispose(); }
}

