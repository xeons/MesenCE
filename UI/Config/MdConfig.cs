using CommunityToolkit.Mvvm.ComponentModel;
using Mesen.Interop;
using System;
using System.Runtime.InteropServices;

namespace Mesen.Config;

public partial class MdConfig : BaseConfig<MdConfig>
{
	[ObservableProperty] public partial ConsoleOverrideConfig ConfigOverrides { get; set; } = new();

	[ObservableProperty] public partial ControllerConfig Port1 { get; set; } = new();
	[ObservableProperty] public partial ControllerConfig Port2 { get; set; } = new();

	[ObservableProperty] public partial bool AllowInvalidInput { get; set; } = false;

	[ValidValues(ConsoleRegion.Auto, ConsoleRegion.Ntsc, ConsoleRegion.NtscJapan, ConsoleRegion.Pal)]
	[ObservableProperty] public partial ConsoleRegion Region { get; set; } = ConsoleRegion.Auto;

	[ObservableProperty] public partial RamState RamPowerOnState { get; set; } = RamState.Random;
	[ObservableProperty] public partial MdFmChip FmChip { get; set; } = MdFmChip.Ym2612;

	[ObservableProperty] public partial bool RemoveSpriteLimit { get; set; } = false;
	[ObservableProperty] public partial bool DisableSprites { get; set; } = false;
	[ObservableProperty] public partial bool DisablePlaneA { get; set; } = false;
	[ObservableProperty] public partial bool DisablePlaneB { get; set; } = false;
	[ObservableProperty] public partial bool DisableWindow { get; set; } = false;
	[ObservableProperty] public partial bool ShowHorizontalBorder { get; set; } = false;
	[ObservableProperty] public partial bool ShowVerticalBorder { get; set; } = false;

	[ObservableProperty][MinMax(0, 100)] public partial UInt32 FmVolume { get; set; } = 100;
	[ObservableProperty][MinMax(0, 100)] public partial UInt32 PsgVolume { get; set; } = 100;

	[ObservableProperty] public partial OverscanConfig NtscOverscan { get; set; } = new();
	[ObservableProperty] public partial OverscanConfig PalOverscan { get; set; } = new();

	public void ApplyConfig()
	{
		ConfigApi.SetMdConfig(new InteropMdConfig() {
			Port1 = Port1.ToInterop(),
			Port2 = Port2.ToInterop(),

			Region = Region,
			RamPowerOnState = RamPowerOnState,
			FmChip = FmChip,

			AllowInvalidInput = AllowInvalidInput,
			RemoveSpriteLimit = RemoveSpriteLimit,
			DisableSprites = DisableSprites,
			DisablePlaneA = DisablePlaneA,
			DisablePlaneB = DisablePlaneB,
			DisableWindow = DisableWindow,
			ShowHorizontalBorder = ShowHorizontalBorder,
			ShowVerticalBorder = ShowVerticalBorder,

			FmVolume = FmVolume,
			PsgVolume = PsgVolume,

			NtscOverscan = NtscOverscan.ToInterop(),
			PalOverscan = PalOverscan.ToInterop(),
		});
	}

	internal void InitializeDefaults(DefaultKeyMappingType defaultMappings)
	{
		Port1.InitDefaults(defaultMappings, ControllerType.MdController3Button);
		Port2.Type = ControllerType.MdController3Button;
	}
}

[StructLayout(LayoutKind.Sequential)]
public struct InteropMdConfig
{
	public InteropControllerConfig Port1;
	public InteropControllerConfig Port2;

	public ConsoleRegion Region;
	public RamState RamPowerOnState;
	public MdFmChip FmChip;

	[MarshalAs(UnmanagedType.I1)] public bool AllowInvalidInput;
	[MarshalAs(UnmanagedType.I1)] public bool RemoveSpriteLimit;
	[MarshalAs(UnmanagedType.I1)] public bool DisableSprites;
	[MarshalAs(UnmanagedType.I1)] public bool DisablePlaneA;
	[MarshalAs(UnmanagedType.I1)] public bool DisablePlaneB;
	[MarshalAs(UnmanagedType.I1)] public bool DisableWindow;
	[MarshalAs(UnmanagedType.I1)] public bool ShowHorizontalBorder;
	[MarshalAs(UnmanagedType.I1)] public bool ShowVerticalBorder;

	public UInt32 FmVolume;
	public UInt32 PsgVolume;

	public InteropOverscanDimensions NtscOverscan;
	public InteropOverscanDimensions PalOverscan;
}

public enum MdFmChip : byte
{
	Ym2612,
	Ym3438
}
