// Local visual prototype only. Never calls settings saves, relay operations,
// network checks or plugin installation. Not installed or enabled by default.
#define wWinMain soundfork_unused_control_entry
#include "../../src/control-app/control_main.cpp"
#undef wWinMain
#include <stdexcept>
#include <iostream>

namespace {
constexpr int kTabs=4100, kChannels=4101, kCustom=4102, kPreviewLanguage=4103;
constexpr wchar_t kCustomClass[]=L"SoundForkPreviewChannelDialog";
LRESULT preview_control_background(WPARAM wparam, LPARAM lparam) {
  const auto dc=reinterpret_cast<HDC>(wparam);
  SetBkColor(dc,GetSysColor(COLOR_WINDOW));
  SetTextColor(dc,GetSysColor(IsWindowEnabled(reinterpret_cast<HWND>(lparam))?COLOR_WINDOWTEXT:COLOR_GRAYTEXT));
  return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
}
struct ChannelDialog {
  HWND left=nullptr,right=nullptr;
  soundfork::capture::ChannelPair pair{};
  bool accepted=false, korean=true;
  HFONT font=nullptr;
};
LRESULT CALLBACK channel_dialog(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
  auto* state=reinterpret_cast<ChannelDialog*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE) {
    state=static_cast<ChannelDialog*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));
  }
  if(!state) return DefWindowProcW(window,message,wparam,lparam);
  if(message==WM_CTLCOLORSTATIC || message==WM_CTLCOLORBTN) return preview_control_background(wparam,lparam);
  if(message==WM_CREATE) {
    for(int side=0;side<2;++side) {
      HWND label=CreateWindowW(L"STATIC",side==0 ? (state->korean?L"왼쪽 출력":L"Left output")
          : (state->korean?L"오른쪽 출력":L"Right output"),WS_CHILD|WS_VISIBLE,24,24+side*44,110,24,window,nullptr,nullptr,nullptr);
      HWND combo=CreateWindowW(L"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,
          146,20+side*44,220,240,window,nullptr,nullptr,nullptr);
      for(unsigned index=0;index<64;++index) {
        const auto text=std::wstring(state->korean?L"출력 ":L"Output ")+std::to_wstring(index+1);
        SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
      }
      SendMessageW(combo,CB_SETCURSEL,side==0?state->pair.left:state->pair.right,0);
      if(side==0) state->left=combo; else state->right=combo;
      set_control_font(label,state->font); set_control_font(combo,state->font);
    }
    HWND note=CreateWindowW(L"STATIC",state->korean?L"선택은 미리보기에만 적용됩니다.":L"Selections apply to this preview only.",
        WS_CHILD|WS_VISIBLE,24,110,350,24,window,nullptr,nullptr,nullptr);
    HWND ok=CreateWindowW(L"BUTTON",state->korean?L"확인":L"OK",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
        164,150,94,30,window,reinterpret_cast<HMENU>(IDOK),nullptr,nullptr);
    HWND cancel=CreateWindowW(L"BUTTON",state->korean?L"취소":L"Cancel",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
        270,150,94,30,window,reinterpret_cast<HMENU>(IDCANCEL),nullptr,nullptr);
    for(auto item:{note,ok,cancel}) set_control_font(item,state->font);
    SetFocus(state->left); return 0;
  }
  if(message==WM_COMMAND && LOWORD(wparam)==IDOK) {
    const auto left=SendMessageW(state->left,CB_GETCURSEL,0,0);
    const auto right=SendMessageW(state->right,CB_GETCURSEL,0,0);
    if(left>=0 && right>=0 && left<64 && right<64) {
      state->pair={static_cast<DWORD>(left),static_cast<DWORD>(right)};
      state->accepted=true; DestroyWindow(window);
    }
    return 0;
  }
  if(message==WM_CLOSE || (message==WM_COMMAND && LOWORD(wparam)==IDCANCEL)) { DestroyWindow(window); return 0; }
  return DefWindowProcW(window,message,wparam,lparam);
}

struct Preview {
  AppState app;
  HWND tabs=nullptr,channels=nullptr,channel_label=nullptr,custom=nullptr;
  HWND obs_title=nullptr,obs_status=nullptr,obs_install=nullptr,obs_remove=nullptr,obs_usage=nullptr;
  HWND settings_title=nullptr,language_label=nullptr,language=nullptr,updates=nullptr,settings_note=nullptr;
  HWND footer=nullptr;
  HWND playback_note=nullptr,buffer_group=nullptr,buffer_note=nullptr;
  soundfork::capture::ChannelPair pair{};
  std::vector<HWND> playback,broadcast,tuning,obs,settings;
  int page=0;
  bool ready=false;
};
void arrange(Preview& p) {
  if(!p.ready) return;
  RECT r{}; GetClientRect(p.app.window,&r);
  const int width=r.right, field=width-198;
  MoveWindow(p.tabs,14,12,width-28,30,TRUE);
  const auto move=[](HWND h,int x,int y,int w,int height){MoveWindow(h,x,y,w,height,TRUE);};
  move(p.app.output_group,14,58,width-28,96);
  move(p.app.backend_combo,30,94,width-170,220); move(p.app.apply_button,width-124,93,94,28);
  move(p.playback_note,30,178,width-60,92); move(p.app.refresh_button,30,288,150,28);
  move(p.app.forwarding_group,14,58,width-28,286);
  move(p.app.relay_enabled,30,84,width-260,26); move(p.app.relay_button,width-220,82,190,30);
  move(p.app.relay_mode_label,30,142,130,24); move(p.app.relay_mode_combo,168,138,field,180);
  move(p.app.endpoint_label,30,190,130,24); move(p.app.endpoint_combo,168,186,field,230);
  move(p.app.startup_checkbox,30,236,width-60,26); move(p.app.vb_cable_button,30,292,174,28);
  move(p.app.note,30,368,width-60,60);
  move(p.buffer_group,14,58,width-28,206);
  move(p.app.buffer_label,30,106,130,24); move(p.app.relay_buffer,168,102,field,230);
  move(p.channel_label,30,162,130,24); move(p.channels,168,158,field-100,240); move(p.custom,width-122,157,92,28);
  move(p.buffer_note,30,290,width-60,112);
  move(p.obs_title,30,72,width-60,28); move(p.obs_status,30,116,width-60,56);
  move(p.obs_install,30,190,210,30); move(p.obs_remove,252,190,110,30);
  move(p.obs_usage,30,252,width-60,164);
  move(p.settings_title,30,72,width-60,28);
  move(p.language_label,30,130,130,24); move(p.language,168,126,240,140);
  move(p.updates,30,188,width-60,28); move(p.settings_note,30,252,width-60,112);
  move(p.footer,18,r.bottom-30,width-36,22);
  for(auto h:p.playback) ShowWindow(h,p.page==0?SW_SHOW:SW_HIDE);
  for(auto h:p.broadcast) ShowWindow(h,p.page==1?SW_SHOW:SW_HIDE);
  for(auto h:p.tuning) ShowWindow(h,p.page==2?SW_SHOW:SW_HIDE);
  for(auto h:p.obs) ShowWindow(h,p.page==3?SW_SHOW:SW_HIDE);
  for(auto h:p.settings) ShowWindow(h,p.page==4?SW_SHOW:SW_HIDE);
  ShowWindow(p.app.backend_label,SW_HIDE);
  ShowWindow(p.app.status,SW_HIDE);
  InvalidateRect(p.app.window,nullptr,TRUE);
}
HWND widget(Preview& p,const wchar_t* kind,const wchar_t* text,int id,DWORD style=0) {
  HWND h=CreateWindowExW(0,kind,text,WS_CHILD|WS_VISIBLE|style,0,0,10,10,p.app.window,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
  if(!h) throw std::runtime_error("preview control creation failed");
  set_control_font(h,p.app.font); return h;
}
void populate_channels(Preview& p) {
  SendMessageW(p.channels,CB_RESETCONTENT,0,0);
  const bool ko=p.app.language==UiLanguage::korean;
  for(unsigned index=0;index<32;++index) {
    const auto text=std::wstring(ko?L"출력 ":L"Outputs ")+std::to_wstring(index*2+1)+L" · "+std::to_wstring(index*2+2);
    SendMessageW(p.channels,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
  }
  const auto custom=std::wstring(ko?L"사용자 지정 — ":L"Custom — ")+std::to_wstring(p.pair.left+1)+L" / "+std::to_wstring(p.pair.right+1);
  SendMessageW(p.channels,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(custom.c_str()));
  SendMessageW(p.channels,CB_SETCURSEL,p.pair.left%2==0 && p.pair.right==p.pair.left+1?p.pair.left/2:32,0);
}
void translate(Preview& p,UiLanguage language) {
  apply_language(&p.app,language,false);
  // Reuse the existing Help menu, detach and destroy Settings for this preview.
  HMENU menu=GetMenu(p.app.window); HMENU settings=GetSubMenu(menu,0);
  RemoveMenu(menu,0,MF_BYPOSITION); DestroyMenu(settings); p.app.obs_menu=nullptr; DrawMenuBar(p.app.window);
  const bool ko=language==UiLanguage::korean;
  SetWindowTextW(p.app.window,ko?L"SoundFork — 간결한 탭 UI 미리보기":L"SoundFork — compact tabs preview");
  const wchar_t* names[]={ko?L"재생":L"Playback",ko?L"방송":L"Broadcast",ko?L"버퍼·채널":L"Buffer / channels",L"OBS",ko?L"설정":L"Settings"};
  for(int i=0;i<5;++i) { TCITEMW item{}; item.mask=TCIF_TEXT; item.pszText=const_cast<wchar_t*>(names[i]);
    SendMessageW(p.tabs,TCM_SETITEMW,static_cast<WPARAM>(i),reinterpret_cast<LPARAM>(&item)); }
  SetWindowTextW(p.channel_label,ko?L"방송 채널":L"Broadcast channels");
  SetWindowTextW(p.custom,ko?L"변경…":L"Edit…");
  SetWindowTextW(p.playback_note,ko?L"게임·DAW에서 SoundFork ASIO를 선택하세요.\r\n재생 출력 변경은 애플리케이션을 다시 시작하면 적용됩니다.":L"Select SoundFork ASIO in your game or DAW.\r\nRestart the application after changing playback output.");
  SetWindowTextW(p.buffer_group,ko?L"방송 오디오 설정":L"Broadcast audio settings");
  SetWindowTextW(p.buffer_note,ko?L"전달 버퍼는 방송용 오디오의 여유량입니다.\r\n표시 시간은 버퍼 길이이며 실측 지연이 아닙니다.\r\n\r\n방송 채널 변경은 게임·DAW 재시작 후 적용됩니다.":L"The forwarding buffer provides headroom for broadcast audio.\r\nIts duration is not measured output latency.\r\n\r\nRestart your game or DAW to apply channel changes.");
  SetWindowTextW(p.obs_title,ko?L"OBS 플러그인":L"OBS plugin");
  const auto obs_version=std::wstring(ko?L"설치된 버전: ":L"Installed version: ")+obs_installed_text(&p.app)+
      (ko?L"\r\n미리보기 포함 파일: ":L"\r\nPreview bundle: ")+ (p.app.obs_status.bundled?p.app.obs_status.bundled_version:(ko?L"없음":L"None"));
  SetWindowTextW(p.obs_status,obs_version.c_str());
  SetWindowTextW(p.obs_install,ko?L"설치 / 업데이트…":L"Install / update…");
  SetWindowTextW(p.obs_remove,ko?L"제거…":L"Remove…");
  SetWindowTextW(p.obs_usage,ko?L"OBS를 다시 시작하고 ‘SoundFork 오디오’ 소스를 추가하세요.\r\n\r\nOBS만 사용한다면 가상 장치나 오디오 전달은 필요하지 않습니다.\r\n\r\n이 비교판에서는 실제 설치·제거를 실행하지 않습니다.":L"Restart OBS and add a SoundFork Audio source.\r\n\r\nOBS-only capture needs no virtual device or audio forwarding.\r\n\r\nThis preview does not install or remove files.");
  SetWindowTextW(p.settings_title,ko?L"일반 설정":L"General settings");
  SetWindowTextW(p.language_label,ko?L"표시 언어":L"Display language");
  SendMessageW(p.language,CB_SETCURSEL,ko?0:1,0);
  SetWindowTextW(p.updates,ko?L"새 버전 자동 알림":L"Notify me about new versions");
  SetWindowTextW(p.settings_note,ko?L"로그인 시 자동 시작은 ‘방송’ 탭에서 설정합니다.\r\n\r\n버전 정보와 진단은 도움말에서 확인할 수 있습니다.\r\n이 미리보기에서는 설정을 저장하지 않습니다.":L"Configure login startup on the Broadcast tab.\r\n\r\nVersion information and diagnostics are in Help.\r\nSettings are not saved in this preview.");
  soundfork::capture::SharedRingObserver observer;
  soundfork::capture::StreamStatus stream;
  const bool active=observer.open() && observer.snapshot(&stream) && stream.producer_active;
  SetWindowTextW(p.footer,ko?(active?L"미리보기 · ASIO 연결됨 · 설정 저장 안 함":L"미리보기 · ASIO 연결 대기 · 설정 저장 안 함")
      :(active?L"Preview · ASIO connected · Settings not saved":L"Preview · Waiting for ASIO · Settings not saved"));
  populate_channels(p); arrange(p);
}
void edit_channels(Preview& p) {
  ChannelDialog data; data.pair=p.pair; data.korean=p.app.language==UiLanguage::korean; data.font=p.app.font;
  RECT owner{}; GetWindowRect(p.app.window,&owner);
  HWND dialog=CreateWindowExW(WS_EX_DLGMODALFRAME|WS_EX_CONTROLPARENT,kCustomClass,
      data.korean?L"방송 채널 사용자 지정":L"Custom broadcast channels",WS_POPUP|WS_CAPTION|WS_SYSMENU,
      owner.left+100,owner.top+130,412,234,p.app.window,nullptr,GetModuleHandleW(nullptr),&data);
  if(!dialog) return;
  EnableWindow(p.app.window,FALSE); ShowWindow(dialog,SW_SHOW);
  MSG message{};
  while(IsWindow(dialog)) {
    const auto result=GetMessageW(&message,nullptr,0,0);
    if(result<=0) { if(result==0) PostQuitMessage(static_cast<int>(message.wParam)); break; }
    if(!IsDialogMessageW(dialog,&message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  if(IsWindow(dialog)) DestroyWindow(dialog);
  EnableWindow(p.app.window,TRUE); SetActiveWindow(p.app.window);
  if(data.accepted) p.pair=data.pair;
  populate_channels(p);
}
LRESULT CALLBACK tabbed_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
  auto* p=reinterpret_cast<Preview*>(GetWindowLongPtrW(window,GWLP_USERDATA));
  if(message==WM_NCCREATE) {
    p=static_cast<Preview*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    p->app.window=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));
  }
  if(!p) return DefWindowProcW(window,message,wparam,lparam);
  if(message==WM_CTLCOLORSTATIC || message==WM_CTLCOLORBTN) return preview_control_background(wparam,lparam);
  if(message==WM_GETMINMAXINFO) { auto* info=reinterpret_cast<MINMAXINFO*>(lparam); info->ptMinTrackSize={650,540}; return 0; }
  if(message==WM_SIZE) { arrange(*p); return 0; }
  if(message==WM_NOTIFY && p->ready) {
    const auto* notify=reinterpret_cast<NMHDR*>(lparam);
    if(notify->hwndFrom==p->tabs && notify->code==TCN_SELCHANGE) {
      p->page=TabCtrl_GetCurSel(p->tabs); arrange(*p); return 0;
    }
  }
  if(message==WM_COMMAND && p->ready) {
    const auto id=LOWORD(wparam);
    if(id==kChannels && HIWORD(wparam)==CBN_SELCHANGE) {
      const auto selected=SendMessageW(p->channels,CB_GETCURSEL,0,0);
      if(selected>=0 && selected<32) { p->pair={static_cast<DWORD>(selected)*2,static_cast<DWORD>(selected)*2+1}; populate_channels(*p); }
      else if(selected==32) edit_channels(*p);
    } else if(id==kCustom) edit_channels(*p);
    else if(id==kPreviewLanguage && HIWORD(wparam)==CBN_SELCHANGE)
      translate(*p,SendMessageW(p->language,CB_GETCURSEL,0,0)==0?UiLanguage::korean:UiLanguage::english);
    else if(id==kShowDiagnostics) show_diagnostics(&p->app);
    else if(id==kAutomaticUpdateNotifications || id==kRelayEnabled || id==kStartupRelay) return 0;
    else if(id==kShowAbout) MessageBoxW(window,L"SoundFork — local tabbed UI prototype",L"SoundFork",MB_OK);
    else if(HIWORD(wparam)==BN_CLICKED || id>=2000)
      MessageBoxW(window,p->app.language==UiLanguage::korean?L"로컬 미리보기입니다. 실제 설정과 오디오는 변경하지 않습니다.":L"Local preview. Actual settings and audio are not changed.",L"SoundFork",MB_OK);
    return 0;
  }
  if(message==WM_CLOSE) { DestroyWindow(window); return 0; }
  if(message==WM_DESTROY) { PostQuitMessage(0); return 0; }
  return DefWindowProcW(window,message,wparam,lparam);
}
void initialize(Preview& p) {
  p.app.force_exit=true; p.app.automatic_updates_enabled=false; p.app.language=UiLanguage::korean;
  create_controls(&p.app); refresh_obs_status(&p.app);
  populate_backend_combo(&p.app); populate_endpoint_combo(&p.app); populate_relay_mode_combo(&p.app); update_status(&p.app);
  p.pair=soundfork::capture::load_channels();
  p.tabs=widget(p,WC_TABCONTROLW,L"",kTabs,WS_TABSTOP);
  for(int i=0;i<5;++i) { TCITEMW item{}; item.mask=TCIF_TEXT; item.pszText=const_cast<wchar_t*>(L" ");
    SendMessageW(p.tabs,TCM_INSERTITEMW,static_cast<WPARAM>(i),reinterpret_cast<LPARAM>(&item)); }
  p.channel_label=widget(p,L"STATIC",L"",0);
  p.channels=widget(p,L"COMBOBOX",L"",kChannels,WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL);
  p.custom=widget(p,L"BUTTON",L"",kCustom,WS_TABSTOP);
  p.obs_title=widget(p,L"STATIC",L"",0); p.obs_status=widget(p,L"STATIC",L"",0);
  p.obs_install=widget(p,L"BUTTON",L"",kObsInstall,WS_TABSTOP);
  p.obs_remove=widget(p,L"BUTTON",L"",kObsRemove,WS_TABSTOP); p.obs_usage=widget(p,L"STATIC",L"",0);
  p.settings_title=widget(p,L"STATIC",L"",0); p.language_label=widget(p,L"STATIC",L"",0);
  p.language=widget(p,L"COMBOBOX",L"",kPreviewLanguage,WS_TABSTOP|CBS_DROPDOWNLIST);
  for(const auto* text:{L"한국어",L"English"}) SendMessageW(p.language,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
  p.updates=widget(p,L"BUTTON",L"",kAutomaticUpdateNotifications,WS_TABSTOP|BS_AUTOCHECKBOX);
  SendMessageW(p.updates,BM_SETCHECK,BST_CHECKED,0);
  p.settings_note=widget(p,L"STATIC",L"",0); p.footer=widget(p,L"STATIC",L"",0);
  p.playback_note=widget(p,L"STATIC",L"",0);
  p.buffer_group=widget(p,L"BUTTON",L"",0,BS_GROUPBOX);
  SetWindowPos(p.buffer_group,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
  p.buffer_note=widget(p,L"STATIC",L"",0);
  p.playback={p.app.output_group,p.app.backend_combo,p.app.apply_button,p.app.refresh_button,p.playback_note};
  p.broadcast={p.app.forwarding_group,
      p.app.relay_mode_label,p.app.relay_mode_combo,p.app.relay_enabled,p.app.relay_button,p.app.endpoint_label,
      p.app.endpoint_combo,p.app.startup_checkbox,p.app.vb_cable_button,p.app.note};
  p.tuning={p.buffer_group,p.buffer_note,p.app.buffer_label,p.app.relay_buffer,p.channel_label,p.channels,p.custom};
  p.obs={p.obs_title,p.obs_status,p.obs_install,p.obs_remove,p.obs_usage};
  p.settings={p.settings_title,p.language_label,p.language,p.updates,p.settings_note};
  p.ready=true; translate(p,UiLanguage::korean);
}
void require(bool condition,const char* why) { if(!condition) throw std::runtime_error(why); }
}
int wmain(int argc,wchar_t**) {
  if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 2;
  struct End { ~End(){CoUninitialize();} } end;
  try {
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_TAB_CLASSES}; InitCommonControlsEx(&common);
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance=GetModuleHandleW(nullptr); wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));
    wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1); wc.lpfnWndProc=tabbed_proc; wc.lpszClassName=L"SoundForkTabbedPreview";
    require(RegisterClassExW(&wc)!=0,"main class");
    wc.lpfnWndProc=channel_dialog; wc.lpszClassName=kCustomClass; require(RegisterClassExW(&wc)!=0,"channel class");
    wc.lpfnWndProc=diagnostic_proc; wc.lpszClassName=kDiagnosticClass; require(RegisterClassExW(&wc)!=0,"diagnostic class");
    Preview p;
    HWND window=CreateWindowExW(WS_EX_CONTROLPARENT,L"SoundForkTabbedPreview",L"SoundFork",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,680,560,nullptr,nullptr,wc.hInstance,&p);
    require(window!=nullptr,"main window"); initialize(p);
    if(argc>1) {
      for(int language=0;language<2;++language) {
        translate(p,language==0?UiLanguage::korean:UiLanguage::english);
        SetWindowPos(window,nullptr,0,0,650,540,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        const wchar_t* expected_ko[]={L"재생",L"방송",L"버퍼·채널",L"OBS",L"설정"};
        const wchar_t* expected_en[]={L"Playback",L"Broadcast",L"Buffer / channels",L"OBS",L"Settings"};
        for(int tab=0;tab<5;++tab) {
          wchar_t title[32]{}; TCITEMW item{}; item.mask=TCIF_TEXT; item.pszText=title; item.cchTextMax=32;
          require(SendMessageW(p.tabs,TCM_GETITEMW,static_cast<WPARAM>(tab),reinterpret_cast<LPARAM>(&item))!=0,"tab label query");
          require(std::wstring_view(title)==(language==0?expected_ko[tab]:expected_en[tab]),"Unicode tab label corrupted");
        }
        for(int page=0;page<5;++page) {
          p.page=page; TabCtrl_SetCurSel(p.tabs,page); arrange(p);
          require((GetWindowLongPtrW(p.app.backend_combo,GWL_STYLE)&WS_VISIBLE)!=0 ? page==0 : page!=0,"playback visibility");
          require((GetWindowLongPtrW(p.app.startup_checkbox,GWL_STYLE)&WS_VISIBLE)!=0 ? page==1 : page!=1,"broadcast visibility");
          require((GetWindowLongPtrW(p.channels,GWL_STYLE)&WS_VISIBLE)!=0 ? page==2 : page!=2,"buffer/channel visibility");
          require((GetWindowLongPtrW(p.obs_install,GWL_STYLE)&WS_VISIBLE)!=0 ? page==3 : page!=3,"OBS page visibility");
          require((GetWindowLongPtrW(p.language,GWL_STYLE)&WS_VISIBLE)!=0 ? page==4 : page!=4,"settings visibility");
          RECT client{}; GetClientRect(window,&client);
          for(const auto* controls:{&p.playback,&p.broadcast,&p.tuning,&p.obs,&p.settings}) {
            for(auto control:*controls) {
              if((GetWindowLongPtrW(control,GWL_STYLE)&WS_VISIBLE)==0) continue;
              RECT bounds{}; GetWindowRect(control,&bounds);
              MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);
              require(bounds.left>=0 && bounds.right<=client.right && bounds.top>=50 && bounds.bottom<=client.bottom-32,
                      "compact page control clipped or overlaps footer");
            }
          }
        }
      }
      ChannelDialog data; data.font=p.app.font;
      HWND dialog=CreateWindowExW(0,kCustomClass,L"Test",WS_OVERLAPPED,0,0,412,234,window,nullptr,wc.hInstance,&data);
      require(dialog!=nullptr,"custom dialog");
      SendMessageW(data.left,CB_SETCURSEL,63,0); SendMessageW(data.right,CB_SETCURSEL,2,0);
      SendMessageW(dialog,WM_COMMAND,IDOK,0);
      require(data.accepted && data.pair.left==63 && data.pair.right==2,"custom channel selection");
      DestroyWindow(window); std::cout<<"Tabbed preview: pages, languages and custom channel dialog passed\n";
    } else {
      ShowWindow(window,SW_SHOW); MSG message{};
      while(GetMessageW(&message,nullptr,0,0)>0) {
        if(p.app.diagnostic_window && IsDialogMessageW(p.app.diagnostic_window,&message)) continue;
        if(IsDialogMessageW(window,&message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
      }
    }
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
  return 0;
}
