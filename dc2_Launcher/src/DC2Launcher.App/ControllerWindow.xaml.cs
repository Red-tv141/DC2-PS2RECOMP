using System.Windows;
using System.Windows.Input;
using DC2Launcher.App.ViewModels;

namespace DC2Launcher.App;

public partial class ControllerWindow : Window
{
    public ControllerWindow(ControllerViewModel viewModel)
    {
        InitializeComponent();
        DataContext = viewModel;
        viewModel.CloseAction = () => Close();
    }

    private void Window_KeyDown(object sender, KeyEventArgs e)
    {
        if (DataContext is ControllerViewModel vm && vm.IsCapturingInput)
        {
            vm.HandleKeyDown(e.Key == Key.System ? e.SystemKey : e.Key);
            e.Handled = true;
        }
    }
}
