using Avalonia;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;
using Mesen.Config;
using Mesen.Debugger.Controls;
using Mesen.Interop;
using Mesen.Utilities;
using CommunityToolkit.Mvvm.ComponentModel;
using System;

namespace Mesen.Debugger.Windows
{
	public class ColorIndexPickerWindow : MesenWindow
	{
		public UInt32[] Palette { get; set; } = Array.Empty<UInt32>();
		public int SelectedPalette { get; set; }
		public int BlockSize { get; set; } = 24;
		public int ColumnCount { get; set; } = 16;

		[Obsolete("For designer only")]
		public ColorIndexPickerWindow() : this(CpuType.Nes, 0) { }

		public ColorIndexPickerWindow(CpuType cpuType, int selectedPalette)
		{
			SelectedPalette = selectedPalette;
			switch(cpuType) {
				case CpuType.Nes:
					Palette = ConfigManager.Config.Nes.UserPalette;
					break;

				case CpuType.Pce:
					Palette = ConfigManager.Config.PcEngine.Palette;
					break;

				case CpuType.Gameboy:
					if(selectedPalette < 4) {
						Palette = ConfigManager.Config.Gameboy.BgColors;
					} else if(selectedPalette < 8) {
						Palette = ConfigManager.Config.Gameboy.Obj0Colors;
					} else {
						Palette = ConfigManager.Config.Gameboy.Obj1Colors;
					}
					ColumnCount = 4;
					SelectedPalette = selectedPalette % 4;
					break;

				case CpuType.Sms:
					Palette = GenerateSmsPalette();
					ColumnCount = 8;
					break;

				case CpuType.Ws:
					Palette = GenerateWsPalette();
					ColumnCount = 8;
					break;

				case CpuType.Md:
					Palette = GenerateMdPalette();
					ColumnCount = 32;
					BlockSize = 16;
					break;

				default:
					throw new NotImplementedException();
			}

			InitializeComponent();
		}

		private static UInt32[] GenerateMdPalette()
		{
			//Index is the raw 9-bit color value (BBBGGGRRR), converted using the VDP's DAC output levels
			byte[] levels = new byte[] { 0, 52, 87, 116, 144, 172, 206, 255 };
			UInt32[] pal = new UInt32[0x200];
			for(int i = 0; i < 0x200; i++) {
				byte r = levels[i & 0x07];
				byte g = levels[(i >> 3) & 0x07];
				byte b = levels[(i >> 6) & 0x07];
				pal[i] = 0xFF000000 | (UInt32)(r << 16) | (UInt32)(g << 8) | b;
			}
			return pal;
		}

		private static UInt32[] GenerateWsPalette()
		{
			UInt32[] pal = new UInt32[8];
			WsState state = DebugApi.GetConsoleState<WsState>(ConsoleType.Ws);
			for(int i = 0; i < 8; i++) {
				int b = state.Ppu.BwShades[i] ^ 0x0F;
				pal[i] = 0xFF000000 | (UInt32)(b | (b << 4) | (b << 8) | (b << 12) | (b << 16) | (b << 20) | (b << 24));
			}
			return pal;
		}

		private static UInt32[] GenerateSmsPalette()
		{
			UInt32[] pal = new UInt32[0x40];
			for(int i = 0; i < 0x40; i++) {
				pal[i] = Rgb222ToArgb((byte)i);
			}
			return pal;
		}

		private static byte Rgb222To8Bit(byte color)
		{
			return (byte)((byte)(color << 6) | (byte)(color << 4) | (byte)(color << 2) | color);
		}

		private static UInt32 Rgb222ToArgb(byte rgb222)
		{
			byte b = Rgb222To8Bit((byte)(rgb222 >> 4));
			byte g = Rgb222To8Bit((byte)((rgb222 >> 2) & 0x3));
			byte r = Rgb222To8Bit((byte)(rgb222 & 0x3));

			return 0xFF000000 | (UInt32)(r << 16) | (UInt32)(g << 8) | b;
		}

		private void InitializeComponent()
		{
			AvaloniaXamlLoader.Load(this);
		}

		protected override void OnOpened(EventArgs e)
		{
			base.OnOpened(e);
			if(Owner != null) {
				WindowExtensions.CenterWindow(this, Owner);
			}
		}

		private void Cancel_OnClick(object sender, RoutedEventArgs e)
		{
			Close(null!);
		}

		private void PaletteColor_OnClick(object sender, PaletteSelector.ColorClickEventArgs e)
		{
			Close(e.ColorIndex);
		}
	}
}
