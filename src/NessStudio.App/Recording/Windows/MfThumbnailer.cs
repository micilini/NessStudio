using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows.Media;
using System.Windows.Media.Imaging;
namespace NessStudio.Recording.Windows
{
    public static class MfThumbnailer
    {
        public static bool TryWriteFramePng(string mp4Path, string pngPath, double seekSeconds = 2.0)
        {
            if (string.IsNullOrWhiteSpace(mp4Path) || !File.Exists(mp4Path))
                return false;
            Directory.CreateDirectory(Path.GetDirectoryName(pngPath)!);
            NessStudio.ViewModel.Helpers.DebugLog.Write(
            $"[MfThumbnailer] TryWriteFramePng | mp4={mp4Path} | png={pngPath} | seekSeconds={seekSeconds:F2}");
            NessStudio.Recording.Windows.MediaFoundationSession.Acquire();
            int hr = 0;
            IMFSourceReader reader = null;
            IMFAttributes readerAttrs = null;
            try
            {
                hr = MFCreateAttributes(out readerAttrs, 1);
                if (hr < 0 || readerAttrs == null)
                    return false;
                Guid gEnableVideoProcessing = MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING;
                hr = readerAttrs.SetUINT32(ref gEnableVideoProcessing, 1);
                if (hr < 0)
                    return false;
                hr = MFCreateSourceReaderFromURL(mp4Path, readerAttrs, out reader);
                if (hr < 0 || reader == null)
                    return false;
                IMFMediaType req = null;
                try
                {
                    hr = MFCreateMediaType(out req);
                    if (hr < 0 || req == null)
                        return false;
                    Guid gMajorType = MF_MT_MAJOR_TYPE;
                    hr = req.SetGUID(ref gMajorType, MFMediaType_Video);
                    if (hr < 0)
                        return false;
                    bool accepted = false;
                    Guid[] candidates = new[]
                    {
                MFVideoFormat_RGB32,
                MFVideoFormat_ARGB32
            };
                    foreach (var candidate in candidates)
                    {
                        Guid gSubtype = MF_MT_SUBTYPE;
                        hr = req.SetGUID(ref gSubtype, candidate);
                        if (hr < 0)
                            continue;
                        try
                        {
                            hr = reader.SetCurrentMediaType(
                                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                IntPtr.Zero,
                                req);
                            if (hr >= 0)
                            {
                                accepted = true;
                                break;
                            }
                        }
                        catch (COMException)
                        {
                        }
                    }
                    if (!accepted)
                        return false;
                }
                finally
                {
                    SafeRelease(req);
                }
                IMFMediaType currentType = null;
                int width = 0;
                int height = 0;
                try
                {
                    hr = reader.GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, out currentType);
                    if (hr < 0 || currentType == null)
                        return false;
                    Guid gFrameSize = MF_MT_FRAME_SIZE;
                    long packedSize;
                    hr = currentType.GetUINT64(ref gFrameSize, out packedSize);
                    if (hr < 0)
                        return false;
                    width = (int)((ulong)packedSize >> 32);
                    height = (int)((ulong)packedSize & 0xFFFFFFFF);
                    if (width <= 0 || height <= 0)
                        return false;
                }
                finally
                {
                    SafeRelease(currentType);
                }
                double normalizedSeekSeconds = seekSeconds;
                if (double.IsNaN(normalizedSeekSeconds) || double.IsInfinity(normalizedSeekSeconds))
                    normalizedSeekSeconds = 2.0;
                if (normalizedSeekSeconds < 0.25)
                    normalizedSeekSeconds = 0.25;

                long targetTimeHns = (long)(normalizedSeekSeconds * 10_000_000d);
                Guid gNullTimeFormat = Guid.Empty;
                try
                {
                    hr = reader.SetCurrentPosition(ref gNullTimeFormat, ref targetTimeHns);
                    if (hr < 0)
                    {
                        NessStudio.ViewModel.Helpers.DebugLog.Write($"[MfThumbnailer] SetCurrentPosition failed hr=0x{hr:X8}, fallback to reader progression");
                    }
                    else
                    {
                        NessStudio.ViewModel.Helpers.DebugLog.Write($"[MfThumbnailer] SetCurrentPosition OK | targetHns={targetTimeHns}");
                    }
                }
                catch
                {
                }
                int attempts = 0;
                while (attempts < 120)
                {
                    attempts++;
                    IMFSample sample = null;
                    hr = reader.ReadSample(
                        MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                        0,
                        out _,
                        out int streamFlags,
                        out long timestamp,
                        out sample);
                    if (hr < 0)
                        return false;
                    if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0)
                    {
                        SafeRelease(sample);
                        return false;
                    }
                    if (sample == null)
                        continue;
                    try
                    {
                        IMFMediaBuffer buffer = null;
                        try
                        {
                            hr = sample.ConvertToContiguousBuffer(out buffer);
                            if (hr < 0 || buffer == null)
                                return false;
                            hr = buffer.Lock(out IntPtr pData, out _, out int currentLength);
                            if (hr < 0)
                                return false;
                            try
                            {
                                int stride = width * 4;
                                int needed = stride * height;
                                if (currentLength < needed)
                                    continue;
                                bool allBlack = true;
                                int checkBytes = Math.Min(currentLength, 4096);
                                unsafe
                                {
                                    byte* ptr = (byte*)pData.ToPointer();
                                    for (int i = 0; i < checkBytes; i++)
                                    {
                                        if (ptr[i] != 0)
                                        {
                                            allBlack = false;
                                            break;
                                        }
                                    }
                                }
                                if (allBlack && attempts < 119)
                                    continue;
                                byte[] managedPixels = new byte[needed];
                                Marshal.Copy(pData, managedPixels, 0, needed);
                                for (int i = 3; i < managedPixels.Length; i += 4)
                                {
                                    managedPixels[i] = 255;
                                }
                                var bitmap = BitmapSource.Create(
                                    width,
                                    height,
                                    96,
                                    96,
                                    PixelFormats.Bgra32,
                                    null,
                                    managedPixels,
                                    stride);
                                var encoder = new PngBitmapEncoder();
                                encoder.Frames.Add(BitmapFrame.Create(bitmap));
                                using var fs = new FileStream(pngPath, FileMode.Create, FileAccess.Write, FileShare.Read);
                                encoder.Save(fs);
                                return true;
                            }
                            finally
                            {
                                try { buffer.Unlock(); } catch { }
                            }
                        }
                        finally
                        {
                            SafeRelease(buffer);
                        }
                    }
                    finally
                    {
                        SafeRelease(sample);
                    }
                }
                return false;
            }
            catch
            {
                return false;
            }
            finally
            {
                SafeRelease(readerAttrs);
                SafeRelease(reader);
                try { NessStudio.Recording.Windows.MediaFoundationSession.Release(); } catch { }
            }
        }
        private const int MF_VERSION = 0x00020070;
        private const int MFSTARTUP_FULL = 0;
        private const int MF_SOURCE_READER_FIRST_VIDEO_STREAM = unchecked((int)0xFFFFFFFC);
        private const int MF_SOURCE_READERF_ENDOFSTREAM = 0x00000001;
        private static readonly Guid MF_MT_MAJOR_TYPE = new("48eba18e-f8c9-4687-bf11-0a74c9f96a8f");
        private static readonly Guid MF_MT_SUBTYPE = new("f7e34c9a-42e8-4714-b74b-cb29d72c35e5");
        private static readonly Guid MF_MT_FRAME_SIZE = new("1652c33d-d6b2-4012-b834-72030849a37d");
        private static readonly Guid MFMediaType_Video = new("73646976-0000-0010-8000-00aa00389b71");
        private static readonly Guid MFVideoFormat_RGB32 = new("00000016-0000-0010-8000-00aa00389b71");
        private static readonly Guid MFVideoFormat_ARGB32 = new("00000015-0000-0010-8000-00aa00389b71");
        private static readonly Guid MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING =
            new("FB394F3D-CCF1-42EE-BBB3-F9B845D5681D");
        [ComImport, Guid("2CD2D921-C447-44A7-A13C-4ADABFC247E3"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMFAttributes
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
        }
        [ComImport, Guid("70ae66f2-c809-4e4f-8915-bdcb406b7993"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMFSourceReader
        {
            int GetStreamSelection(int dwStreamIndex, out bool pfSelected);
            int SetStreamSelection(int dwStreamIndex, bool fSelected);
            int GetNativeMediaType(int dwStreamIndex, int dwMediaTypeIndex, out IMFMediaType ppMediaType);
            int GetCurrentMediaType(int dwStreamIndex, out IMFMediaType ppMediaType);
            int SetCurrentMediaType(int dwStreamIndex, IntPtr pdwReserved, IMFMediaType pMediaType);
            int SetCurrentPosition(ref Guid guidTimeFormat, ref long varPosition);
            int ReadSample(int dwStreamIndex, int dwControlFlags, out int pdwActualStreamIndex, out int pdwStreamFlags, out long pllTimestamp, out IMFSample ppSample);
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
        [ComImport, Guid("c40a00f2-b93a-4d80-ae8c-5a1c634f58e4"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMFSample
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
            int GetSampleFlags(out int pdwSampleFlags);
            int SetSampleFlags(int dwSampleFlags);
            int GetSampleTime(out long phnsSampleTime);
            int SetSampleTime(long hnsSampleTime);
            int GetSampleDuration(out long phnsSampleDuration);
            int SetSampleDuration(long hnsSampleDuration);
            int GetBufferCount(out int pdwBufferCount);
            int GetBufferByIndex(int dwIndex, out IntPtr ppBuffer);
            int ConvertToContiguousBuffer(out IMFMediaBuffer ppBuffer);
            int AddBuffer(IntPtr pBuffer);
            int RemoveBufferByIndex(int dwIndex);
            int RemoveAllBuffers();
            int GetTotalLength(out int pcbTotalLength);
            int CopyToBuffer(IntPtr pBuffer);
        }
        [ComImport, Guid("045FA593-8799-42b8-BC8D-8968C6453507"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
        private interface IMFMediaBuffer
        {
            int Lock(out IntPtr ppbBuffer, out int pcbMaxLength, out int pcbCurrentLength);
            int Unlock();
            int GetCurrentLength(out int pcbCurrentLength);
            int SetCurrentLength(int cbCurrentLength);
            int GetMaxLength(out int pcbMaxLength);
        }
        [DllImport("mfplat.dll", ExactSpelling = true)]
        private static extern int MFStartup(int version, int dwFlags);
        [DllImport("mfplat.dll", ExactSpelling = true)]
        private static extern int MFShutdown();
        [DllImport("mfplat.dll", ExactSpelling = true)]
        private static extern int MFCreateAttributes(out IMFAttributes ppMFAttributes, int cInitialSize);
        [DllImport("mfplat.dll", ExactSpelling = true)]
        private static extern int MFCreateMediaType(out IMFMediaType ppMFType);
        [DllImport("mfreadwrite.dll", ExactSpelling = true, CharSet = CharSet.Unicode)]
        private static extern int MFCreateSourceReaderFromURL(
            string pwszURL,
            IMFAttributes pAttributes,
            out IMFSourceReader ppSourceReader);
        private static void SafeRelease(object com)
        {
            try
            {
                if (com != null)
                    Marshal.ReleaseComObject(com);
            }
            catch
            {
            }
        }
    }
}