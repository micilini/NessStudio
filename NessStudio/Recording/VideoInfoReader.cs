using System;
using System.IO;
using System.Runtime.InteropServices;
namespace NessStudio.Recording
{
    public static class VideoInfoReader
    {
        public sealed class VideoInfo
        {
            public int Width { get; set; }
            public int Height { get; set; }
            public int Fps { get; set; }
            public TimeSpan Duration { get; set; }
        }
        public static VideoInfo ReadMp4Info(string mp4Path)
        {
            if (string.IsNullOrWhiteSpace(mp4Path) || !File.Exists(mp4Path))
                return null;
            int w = 0, h = 0, fps = 0;
            TimeSpan duration = TimeSpan.Zero;
            
            bool isMkv = mp4Path.EndsWith(".mkv", StringComparison.OrdinalIgnoreCase);
            if (isMkv)
            {
                try
                {
                    return ReadMkvInfoFromEbml(mp4Path);
                }
                catch (Exception ex)
                {
                    NessStudio.ViewModel.Helpers.DebugLog.Write(
                        "[VideoInfoReader] ReadMkvInfoFromEbml failed: " + ex.Message);
                    return new VideoInfo { Width = 0, Height = 0, Fps = 30, Duration = TimeSpan.Zero };
                }
            }

            try
            {
                duration = ReadMp4DurationFromAtoms(mp4Path);
            }
            catch
            {
                duration = TimeSpan.Zero;
            }
            NessStudio.Recording.Windows.MediaFoundationSession.Acquire();
            try
            {
                int hr = MFCreateSourceReaderFromURL(mp4Path, IntPtr.Zero, out var reader);
                ThrowIfFailed(hr, "MFCreateSourceReaderFromURL(videoInfo)");
                try
                {
                    hr = reader.GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, out var mt);
                    ThrowIfFailed(hr, "GetCurrentMediaType(videoInfo)");
                    try
                    {
                        Guid kSize = MF_MT_FRAME_SIZE;
                        if (mt.GetUINT64(ref kSize, out long size) >= 0)
                        {
                            w = (int)((ulong)size >> 32);
                            h = (int)((ulong)size & 0xFFFFFFFF);
                        }
                        Guid kRate = MF_MT_FRAME_RATE;
                        if (mt.GetUINT64(ref kRate, out long rate) >= 0)
                        {
                            uint num = (uint)((ulong)rate >> 32);
                            uint den = (uint)((ulong)rate & 0xFFFFFFFF);
                            if (den != 0) fps = (int)Math.Round(num / (double)den);
                        }
                    }
                    finally
                    {
                        SafeRelease(mt);
                    }
                }
                finally
                {
                    SafeRelease(reader);
                }
            }
            finally
            {
                NessStudio.Recording.Windows.MediaFoundationSession.Release();
            }
            return new VideoInfo
            {
                Width = w,
                Height = h,
                Fps = fps,
                Duration = duration
            };
        }

        private static VideoInfo ReadMkvInfoFromEbml(string mkvPath)
        {
            
            int width = 0, height = 0, fps = 30;
            double durationMs = 0;

            using var fs = new FileStream(mkvPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            using var br = new BinaryReader(fs);

            long fileLen = fs.Length;
            if (fileLen < 32) return new VideoInfo { Width = 0, Height = 0, Fps = 30, Duration = TimeSpan.Zero };

            
            long scanLimit = Math.Min(fileLen, 64 * 1024);
            byte[] header = new byte[scanLimit];
            fs.Read(header, 0, header.Length);

            
            int idx = FindEbmlId(header, new byte[] { 0x44, 0x89 });
            if (idx >= 0 && idx + 2 < header.Length)
            {
                int afterId = idx + 2;
                var (sizeLen, dataSize) = ReadVintValue(header, afterId);
                if (sizeLen > 0 && dataSize == 8 && afterId + sizeLen + 8 <= header.Length)
                {
                    int dataStart = afterId + sizeLen;
                    byte[] dBytes = new byte[8];
                    Array.Copy(header, dataStart, dBytes, 0, 8);
                    if (BitConverter.IsLittleEndian) Array.Reverse(dBytes);
                    durationMs = BitConverter.ToDouble(dBytes, 0);
                }
            }

            
            idx = FindEbmlId(header, new byte[] { 0xB0 });
            if (idx >= 0)
            {
                int afterId = idx + 1;
                var (sizeLen, dataSize) = ReadVintValue(header, afterId);
                if (sizeLen > 0 && dataSize <= 4 && afterId + sizeLen + dataSize <= header.Length)
                    width = (int)ReadBEUint(header, afterId + sizeLen, dataSize);
            }

            
            idx = FindEbmlId(header, new byte[] { 0xBA });
            if (idx >= 0)
            {
                int afterId = idx + 1;
                var (sizeLen, dataSize) = ReadVintValue(header, afterId);
                if (sizeLen > 0 && dataSize <= 4 && afterId + sizeLen + dataSize <= header.Length)
                    height = (int)ReadBEUint(header, afterId + sizeLen, dataSize);
            }

            
            idx = FindEbmlId(header, new byte[] { 0x23, 0xE3, 0x83 });
            if (idx >= 0)
            {
                int afterId = idx + 3;
                var (sizeLen, dataSize) = ReadVintValue(header, afterId);
                if (sizeLen > 0 && dataSize <= 8 && afterId + sizeLen + dataSize <= header.Length)
                {
                    ulong durationNs = ReadBEUint(header, afterId + sizeLen, dataSize);
                    if (durationNs > 0) fps = (int)Math.Round(1_000_000_000.0 / durationNs);
                }
            }

            return new VideoInfo
            {
                Width = width,
                Height = height,
                Fps = fps > 0 ? fps : 30,
                Duration = durationMs > 0 ? TimeSpan.FromMilliseconds(durationMs) : TimeSpan.Zero
            };
        }

        private static int FindEbmlId(byte[] data, byte[] id)
        {
            for (int i = 0; i <= data.Length - id.Length; i++)
            {
                bool match = true;
                for (int j = 0; j < id.Length; j++)
                {
                    if (data[i + j] != id[j]) { match = false; break; }
                }
                if (match) return i;
            }
            return -1;
        }

        private static (int sizeLen, int dataSize) ReadVintValue(byte[] data, int offset)
        {
            if (offset >= data.Length) return (0, 0);
            byte first = data[offset];
            if (first == 0) return (0, 0);
            int len = 0;
            for (int i = 7; i >= 0; i--)
            {
                if ((first & (1 << i)) != 0) { len = 8 - i; break; }
            }
            if (len == 0 || offset + len > data.Length) return (0, 0);
            ulong val = (ulong)(first & ((1 << (8 - len)) - 1));
            for (int i = 1; i < len; i++)
                val = (val << 8) | data[offset + i];
            return (len, (int)val);
        }

        private static ulong ReadBEUint(byte[] data, int offset, int len)
        {
            ulong val = 0;
            for (int i = 0; i < len && offset + i < data.Length; i++)
                val = (val << 8) | data[offset + i];
            return val;
        }

        private static TimeSpan ReadMp4DurationFromAtoms(string mp4Path)
        {
            using var fs = new FileStream(mp4Path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            using var br = new BinaryReader(fs);
            while (fs.Position + 8 <= fs.Length)
            {
                long boxStart = fs.Position;
                ulong boxSize = ReadUInt32BE(br);
                string boxType = new string(br.ReadChars(4));
                if (boxSize == 1)
                {
                    boxSize = ReadUInt64BE(br);
                }
                else if (boxSize == 0)
                {
                    boxSize = (ulong)(fs.Length - boxStart);
                }
                if (boxSize < 8)
                    break;
                if (boxType == "moov")
                {
                    long moovEnd = boxStart + (long)boxSize;
                    while (fs.Position + 8 <= moovEnd)
                    {
                        long childStart = fs.Position;
                        ulong childSize = ReadUInt32BE(br);
                        string childType = new string(br.ReadChars(4));
                        if (childSize == 1)
                        {
                            childSize = ReadUInt64BE(br);
                        }
                        else if (childSize == 0)
                        {
                            childSize = (ulong)(moovEnd - childStart);
                        }
                        if (childSize < 8)
                            break;
                        if (childType == "mvhd")
                        {
                            return ReadMvhdDuration(br, childStart, childSize);
                        }
                        fs.Position = childStart + (long)childSize;
                    }
                }
                fs.Position = boxStart + (long)boxSize;
            }
            return TimeSpan.Zero;
        }
        private static TimeSpan ReadMvhdDuration(BinaryReader br, long boxStart, ulong boxSize)
        {
            var fs = br.BaseStream;
            byte version = br.ReadByte();
            br.ReadBytes(3); 
            if (version == 0)
            {
                br.ReadUInt32(); 
                br.ReadUInt32(); 
                uint timescale = ReadUInt32BE(br);
                uint duration = ReadUInt32BE(br);
                if (timescale == 0)
                    return TimeSpan.Zero;
                return TimeSpan.FromSeconds(duration / (double)timescale);
            }
            else if (version == 1)
            {
                ReadUInt64BE(br); 
                ReadUInt64BE(br); 
                uint timescale = ReadUInt32BE(br);
                ulong duration = ReadUInt64BE(br);
                if (timescale == 0)
                    return TimeSpan.Zero;
                return TimeSpan.FromSeconds(duration / (double)timescale);
            }
            fs.Position = boxStart + (long)boxSize;
            return TimeSpan.Zero;
        }
        private static uint ReadUInt32BE(BinaryReader br)
        {
            var b = br.ReadBytes(4);
            if (b.Length < 4) return 0;
            if (BitConverter.IsLittleEndian)
                Array.Reverse(b);
            return BitConverter.ToUInt32(b, 0);
        }
        private static ulong ReadUInt64BE(BinaryReader br)
        {
            var b = br.ReadBytes(8);
            if (b.Length < 8) return 0;
            if (BitConverter.IsLittleEndian)
                Array.Reverse(b);
            return BitConverter.ToUInt64(b, 0);
        }
        private const int MF_VERSION = 0x00020070;
        private const int MFSTARTUP_FULL = 0;
        private const int MF_SOURCE_READER_FIRST_VIDEO_STREAM = unchecked((int)0xFFFFFFFC);
        private static readonly Guid MF_MT_FRAME_SIZE = new("1652c33d-d6b2-4012-b834-72030849a37d");
        private static readonly Guid MF_MT_FRAME_RATE = new("c459a2e8-3d2c-4e44-b132-fee5156c7bb0");
        [ComImport, Guid("70ae66f2-c809-4e4f-8915-bdcb406b7993"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMFSourceReader
        {
            int GetStreamSelection(int dwStreamIndex, out bool pfSelected);
            int SetStreamSelection(int dwStreamIndex, bool fSelected);
            int GetNativeMediaType(int dwStreamIndex, int dwMediaTypeIndex, out IMFMediaType ppMediaType);
            int GetCurrentMediaType(int dwStreamIndex, out IMFMediaType ppMediaType);
            int SetCurrentMediaType(int dwStreamIndex, IntPtr pdwReserved, IMFMediaType pMediaType);
            int SetCurrentPosition(ref Guid guidTimeFormat, ref long varPosition);
            int ReadSample(int dwStreamIndex, int dwControlFlags, out int pdwActualStreamIndex, out int pdwStreamFlags, out long pllTimestamp, out IntPtr ppSample);
            int Flush(int dwStreamIndex);
            int GetServiceForStream(int dwStreamIndex, ref Guid guidService, ref Guid riid, out IntPtr ppvObject);
            int GetPresentationAttribute(int dwStreamIndex, ref Guid guidAttribute, out IntPtr pvAttribute);
        }
        [ComImport, Guid("44ae0fa8-ea31-4109-8d2e-4cae4997c555"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMFMediaType
        {
            int GetItem(ref Guid guidKey, IntPtr pValue);
            int GetItemType(ref Guid guidKey, out int pType);
            int CompareItem(ref Guid guidKey, IntPtr Value, out int pbResult);
            int Compare(IntPtr pTheirs, int MatchType, out int pbResult);
            int GetUINT32(ref Guid guidKey, out int punValue);
            int GetUINT64(ref Guid guidKey, out long punValue);
            int GetDouble(ref Guid guidKey, out double pfValue);
            int GetGUID(ref Guid guidKey, out Guid pguidValue);
            int GetStringLength(ref Guid guidKey, out int pcchLength);
            int GetString(ref Guid guidKey, IntPtr pwszValue, int cchBufSize, out int pcchLength);
            int GetAllocatedString(ref Guid guidKey, out IntPtr ppwszValue, out int pcchLength);
            int GetBlobSize(ref Guid guidKey, out int pcbBlobSize);
            int GetBlob(ref Guid guidKey, IntPtr pBuf, int cbBufSize, out int pcbBlobSize);
            int GetAllocatedBlob(ref Guid guidKey, out IntPtr ip, out int pcbSize);
            int GetUnknown(ref Guid guidKey, ref Guid riid, [MarshalAs(UnmanagedType.IUnknown)] out object ppv);
            int SetItem(ref Guid guidKey, IntPtr Value);
            int DeleteItem(ref Guid guidKey);
            int DeleteAllItems();
            int SetUINT32(ref Guid guidKey, int unValue);
            int SetUINT64(ref Guid guidKey, long unValue);
            int SetDouble(ref Guid guidKey, double fValue);
            int SetGUID(ref Guid guidKey, [MarshalAs(UnmanagedType.LPStruct)] Guid guidValue);
            int SetString(ref Guid guidKey, [MarshalAs(UnmanagedType.LPWStr)] string wszValue);
            int SetBlob(ref Guid guidKey, IntPtr pBuf, int cbBufSize);
            int SetUnknown(ref Guid guidKey, [MarshalAs(UnmanagedType.IUnknown)] object pUnknown);
            int LockStore();
            int UnlockStore();
            int GetCount(out int pcItems);
            int GetItemByIndex(int unIndex, out Guid pguidKey, IntPtr pValue);
            int CopyAllItems(IntPtr pDest);
            int GetMajorType(out Guid pguidMajorType);
            int IsCompressedFormat(out int pfCompressed);
            int IsEqual(IntPtr pIMediaType, out int pdwFlags);
            int GetRepresentation(Guid guidRepresentation, out IntPtr ppvRepresentation);
            int FreeRepresentation(Guid guidRepresentation, IntPtr pvRepresentation);
        }
        [DllImport("mfplat.dll", ExactSpelling = true)]
        private static extern int MFStartup(int version, int dwFlags);
        [DllImport("mfplat.dll", ExactSpelling = true)]
        private static extern int MFShutdown();
        [DllImport("mfreadwrite.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
        private static extern int MFCreateSourceReaderFromURL(string pwszURL, IntPtr pAttributes, out IMFSourceReader ppSourceReader);
        private static void ThrowIfFailed(int hr, string what)
        {
            if (hr < 0) throw new COMException($"{what} failed (hr=0x{hr:X8})", hr);
        }
        private static void SafeRelease(object com)
        {
            try { if (com != null) Marshal.ReleaseComObject(com); } catch { }
        }
    }
}