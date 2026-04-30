using System;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;

namespace NessStudio.Recording.Windows
{
    internal static class SoftwareBitmapPreviewConverter
    {
        public static BitmapSource TryConvert(SoftwareBitmap softwareBitmap)
        {
            if (softwareBitmap == null)
                return null;

            SoftwareBitmap normalized = null;

            try
            {
                normalized =
                    softwareBitmap.BitmapPixelFormat == BitmapPixelFormat.Bgra8 &&
                    softwareBitmap.BitmapAlphaMode == BitmapAlphaMode.Premultiplied
                        ? softwareBitmap
                        : SoftwareBitmap.Convert(
                            softwareBitmap,
                            BitmapPixelFormat.Bgra8,
                            BitmapAlphaMode.Premultiplied);

                int width = normalized.PixelWidth;
                int height = normalized.PixelHeight;
                int stride = width * 4;
                uint capacity = (uint)(stride * height);

                IBuffer buffer = new global::Windows.Storage.Streams.Buffer(capacity);
                normalized.CopyToBuffer(buffer);

                byte[] pixels = new byte[capacity];
                using (var reader = DataReader.FromBuffer(buffer))
                {
                    reader.ReadBytes(pixels);
                }

                var bitmap = BitmapSource.Create(
                    width,
                    height,
                    96,
                    96,
                    PixelFormats.Bgra32,
                    null,
                    pixels,
                    stride);

                bitmap.Freeze();
                return bitmap;
            }
            catch
            {
                return null;
            }
            finally
            {
                if (!ReferenceEquals(normalized, softwareBitmap))
                {
                    try { normalized?.Dispose(); } catch { }
                }
            }
        }
    }
}