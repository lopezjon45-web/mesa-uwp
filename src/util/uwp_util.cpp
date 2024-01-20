#include "uwp_util.h"

#include <atomic>
#include <unknwn.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.System.Profile.h>
#include <winrt/Windows.Graphics.Display.h>
#include <winrt/Windows.Graphics.Display.Core.h>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::System::Profile;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::Graphics::Display::Core;

static CoreWindow _uwpCoreWind = nullptr;
static DisplayInformation _uwpDisplayInfo = nullptr;
static HdmiDisplayInformation _uwpHdi = nullptr;
static HdmiDisplayMode _uwpHdm = nullptr;

static CoreWindow uwp_get_corewindow()
{
   if (!_uwpCoreWind) {
      _uwpCoreWind = CoreWindow::GetForCurrentThread();
   }

   return _uwpCoreWind;
}

extern "C" void* uwp_get_window_reference(void)
{
   CoreWindow window = uwp_get_corewindow();
   if (!window)
      return nullptr;

   return winrt::get_abi(window);
}

static HdmiDisplayInformation uwp_get_hdi()
{
   if (!_uwpHdi) {
      _uwpHdi = HdmiDisplayInformation::GetForCurrentView();
   }

   return _uwpHdi;
}

static HdmiDisplayMode uwp_get_hdm()
{
   if (!_uwpHdm) {
      HdmiDisplayInformation hdi = uwp_get_hdi();
      if (hdi) {
         _uwpHdm = hdi.GetCurrentDisplayMode();
      }
   }

   return _uwpHdm;
}

static DisplayInformation uwp_get_displayInfo()
{
   if (!_uwpDisplayInfo) {
      _uwpDisplayInfo = DisplayInformation::GetForCurrentView();
   }

   return _uwpDisplayInfo;
}

static bool is_running_on_xbox(void)
{
   hstring device_family = AnalyticsInfo::VersionInfo().DeviceFamily();
   return (device_family == L"Windows.Xbox");
}

static int current_height = -1;

extern "C" int uwp_get_height(void)
{
   if (current_height != -1)
      return current_height;

   /* This function must be performed within UI thread,
    * otherwise it will cause a crash in specific cases
    * https://github.com/libretro/RetroArch/issues/13491 */
   float surface_scale = 0;
   int ret = -1;
   std::atomic<bool> finished{false};
   CoreDispatcher dispatcher =
         CoreApplication::MainView().CoreWindow().Dispatcher();
   dispatcher.RunAsync(
         CoreDispatcherPriority::Normal,
         DispatchedHandler([&surface_scale, &ret, &finished]()
            {
               if (is_running_on_xbox())
               {
                  HdmiDisplayMode hdm = uwp_get_hdm();
                  if (hdm)
                     ret = static_cast<int>(hdm.ResolutionHeightInRawPixels());
               }

               if (ret == -1)
               {
                  const LONG32 resolution_scale = static_cast<LONG32>(uwp_get_displayInfo().ResolutionScale());
                  surface_scale                 = static_cast<float>(resolution_scale) / 100.0f;
                  ret                           = static_cast<LONG32>(
                        uwp_get_corewindow().Bounds().Height
                        * surface_scale);
               }
               finished.store(true);
            }));
   while (!finished.load())
   {
      dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
   }
   current_height = ret;
   return ret;
}

static int current_width = -1;

extern "C" int uwp_get_width(void)
{
   if (current_width != -1)
      return current_width;

   /* This function must be performed within UI thread,
    * otherwise it will cause a crash in specific cases
    * https://github.com/libretro/RetroArch/issues/13491 */
   float surface_scale = 0;
   int returnValue = -1;
   std::atomic<bool> finished{false};
   CoreDispatcher dispatcher =
         CoreApplication::MainView().CoreWindow().Dispatcher();
   dispatcher.RunAsync(
         CoreDispatcherPriority::Normal,
         DispatchedHandler([&surface_scale, &returnValue, &finished]()
            {
               if (is_running_on_xbox())
               {
                  HdmiDisplayMode hdm = uwp_get_hdm();
                  if (hdm)
                     returnValue = static_cast<int>(hdm.ResolutionWidthInRawPixels());
               }

               if (returnValue == -1)
               {
                  const LONG32 resolution_scale = static_cast<LONG32>(uwp_get_displayInfo().ResolutionScale());
                  surface_scale = static_cast<float>(resolution_scale) / 100.0f;
                  returnValue   = static_cast<LONG32>(
                        uwp_get_corewindow().Bounds().Width
                        * surface_scale);
               }
               finished.store(true);
            }));
   while (!finished.load())
   {
      dispatcher.ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
   }

   current_width = returnValue;
   return returnValue;
}
