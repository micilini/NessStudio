using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using NessStudio.Interop.Native;

namespace NessStudio.Presentation;

/// <summary>Owns a stable WPF render target on the preview's D3D9Ex device.</summary>
internal sealed unsafe class D3DPreviewSurface : IDisposable
{
    private sealed class ComHandle() : SafeHandleZeroOrMinusOneIsInvalid(true)
    {
        internal nint Pointer { get { ObjectDisposedException.ThrowIf(IsClosed, this); return handle; } }
        protected override bool ReleaseHandle() { Marshal.Release(handle); return true; }
        internal static ComHandle Own(nint pointer) { var owned = new ComHandle(); owned.SetHandle(pointer); return owned; }
    }

    private ComHandle? _device, _surface, _completion;
    internal ulong Generation { get; }
    internal int Width { get; }
    internal int Height { get; }
    internal nint Pointer => _surface!.Pointer;

    internal D3DPreviewSurface(NativePreviewFrame frame)
    {
        Generation = frame.Generation;
        Width = frame.Width;
        Height = frame.Height;
        try
        {
            nint device = 0;
            // IDirect3DSurface9::GetDevice; owns the returned COM reference.
            Check(((delegate* unmanaged[Stdcall]<nint, nint*, int>)Table(frame.Pointer)[3])(frame.Pointer, &device));
            _device = ComHandle.Own(device);
            nint surface = 0;
            // IDirect3DDevice9::CreateRenderTarget, A8R8G8B8, no multisampling, non-lockable.
            Check(((delegate* unmanaged[Stdcall]<nint, uint, uint, uint, uint, uint, int, nint*, nint, int>)Table(device)[28])
                (device, (uint)Width, (uint)Height, 21, 0, 0, 0, &surface, 0));
            _surface = ComHandle.Own(surface);
            nint completion = 0;
            // IDirect3DDevice9::CreateQuery, D3DQUERYTYPE_EVENT.
            Check(((delegate* unmanaged[Stdcall]<nint, uint, nint*, int>)Table(device)[118])(device, 8, &completion));
            _completion = ComHandle.Own(completion);
        }
        catch { Dispose(); throw; }
    }

    /// <summary>Copies only presentation pixels. The source lease must survive the completion query.</summary>
    internal void CopyFrom(NativePreviewFrame frame)
    {
        if (frame.Generation != Generation || frame.Width != Width || frame.Height != Height)
            throw new InvalidOperationException("Preview surface generation changed during presentation.");
        nint device = _device!.Pointer;
        // IDirect3DDevice9::StretchRect, identical geometry and no filtering.
        Check(((delegate* unmanaged[Stdcall]<nint, nint, nint, nint, nint, uint, int>)Table(device)[34])
            (device, frame.Pointer, 0, Pointer, 0, 0));
        nint query = _completion!.Pointer;
        // IDirect3DQuery9::Issue, D3DISSUE_END.
        Check(((delegate* unmanaged[Stdcall]<nint, uint, int>)Table(query)[6])(query, 1));
    }

    internal bool IsCopyComplete()
    {
        nint query = _completion!.Pointer;
        // Poll without waiting; FLUSH submits the copy on the presentation device.
        int result = ((delegate* unmanaged[Stdcall]<nint, nint, uint, uint, int>)Table(query)[7])(query, 0, 0, 1);
        Check(result);
        return result == 0;
    }

    private static nint* Table(nint value) => *(nint**)value;
    private static void Check(int result) { if (result < 0) Marshal.ThrowExceptionForHR(result); }

    public void Dispose()
    {
        _completion?.Dispose(); _completion = null;
        _surface?.Dispose(); _surface = null;
        _device?.Dispose(); _device = null;
    }
}
