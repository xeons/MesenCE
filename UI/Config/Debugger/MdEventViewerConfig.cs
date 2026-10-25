using Avalonia.Media;
using Mesen.Interop;
using Mesen.ViewModels;
using CommunityToolkit.Mvvm.ComponentModel;

namespace Mesen.Config
{
	public partial class MdEventViewerConfig : ViewModelBase
	{
		[ObservableProperty] public partial EventViewerCategoryCfg VdpDataWrite { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xB4, 0x7A, 0xDA));
		[ObservableProperty] public partial EventViewerCategoryCfg VdpDataRead { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xE2, 0x51, 0xF7));
		[ObservableProperty] public partial EventViewerCategoryCfg VdpControlWrite { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0x00, 0x75, 0x97));
		[ObservableProperty] public partial EventViewerCategoryCfg VdpControlRead { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xD1, 0xDD, 0x42));
		[ObservableProperty] public partial EventViewerCategoryCfg VdpHvCounterRead { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0x53, 0xD7, 0x44));

		[ObservableProperty] public partial EventViewerCategoryCfg PsgWrite { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xFF, 0x5E, 0x5E));
		[ObservableProperty] public partial EventViewerCategoryCfg FmWrite { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xC9, 0x29, 0x29));
		[ObservableProperty] public partial EventViewerCategoryCfg FmRead { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xF9, 0xA0, 0x60));

		[ObservableProperty] public partial EventViewerCategoryCfg Z80RamWrite { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0x4A, 0x7C, 0xD9));
		[ObservableProperty] public partial EventViewerCategoryCfg Z80RamRead { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0x9F, 0x93, 0xC6));
		[ObservableProperty] public partial EventViewerCategoryCfg Z80BankWrite { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0x4A, 0xFE, 0xAC));
		[ObservableProperty] public partial EventViewerCategoryCfg Z80BusControl { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xF9, 0xFE, 0xAC));

		[ObservableProperty] public partial EventViewerCategoryCfg IoWrite { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0x18, 0x98, 0xE4));
		[ObservableProperty] public partial EventViewerCategoryCfg IoRead { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xAC, 0xFE, 0xAC));
		[ObservableProperty] public partial EventViewerCategoryCfg CartRegisterWrite { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xFE, 0x78, 0x7B));

		[ObservableProperty] public partial EventViewerCategoryCfg Irq { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0xC4, 0xF4, 0x7A));
		[ObservableProperty] public partial EventViewerCategoryCfg MarkedBreakpoints { get; set; } = new EventViewerCategoryCfg(Color.FromRgb(0x18, 0x98, 0xE4));

		[ObservableProperty] public partial bool ShowPreviousFrameEvents { get; set; } = true;
		[ObservableProperty] public partial bool ShowZ80Events { get; set; } = true;

		public InteropMdEventViewerConfig ToInterop()
		{
			return new InteropMdEventViewerConfig() {
				Irq = this.Irq,
				MarkedBreakpoints = this.MarkedBreakpoints,

				VdpDataWrite = this.VdpDataWrite,
				VdpDataRead = this.VdpDataRead,
				VdpControlWrite = this.VdpControlWrite,
				VdpControlRead = this.VdpControlRead,
				VdpHvCounterRead = this.VdpHvCounterRead,

				PsgWrite = this.PsgWrite,
				FmWrite = this.FmWrite,
				FmRead = this.FmRead,

				Z80RamWrite = this.Z80RamWrite,
				Z80RamRead = this.Z80RamRead,
				Z80BankWrite = this.Z80BankWrite,
				Z80BusControl = this.Z80BusControl,

				IoWrite = this.IoWrite,
				IoRead = this.IoRead,
				CartRegisterWrite = this.CartRegisterWrite,

				ShowPreviousFrameEvents = this.ShowPreviousFrameEvents,
				ShowZ80Events = this.ShowZ80Events
			};
		}
	}
}
