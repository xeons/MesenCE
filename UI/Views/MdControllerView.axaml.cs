using Avalonia.Controls;
using Avalonia.Markup.Xaml;

namespace Mesen.Views
{
	public class MdControllerView : UserControl
	{
		public bool ShowSixButtons { get; }

		public MdControllerView() : this(false)
		{
		}

		public MdControllerView(bool showSixButtons)
		{
			ShowSixButtons = showSixButtons;

			InitializeComponent();
		}

		private void InitializeComponent()
		{
			AvaloniaXamlLoader.Load(this);
		}
	}
}
