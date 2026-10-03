/* UIKit controls feed a standard SDL gamepad. The first hardware controller
   shares player one with touch; additional hardware controllers keep their ports. */
#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>
#include "ios_host.h"
#import "host_orientation.h"
#include <math.h>

static SDL_Joystick *touch_joystick;
static SDL_JoystickID touch_id, primary_hardware;

static void button_state(int button, BOOL down) {
    if(touch_joystick) SDL_SetJoystickVirtualButton(touch_joystick,button,down);
}
static void axis_state(int axis, float value) {
    if(touch_joystick) SDL_SetJoystickVirtualAxis(touch_joystick,axis,(Sint16)(fmaxf(-1,fminf(1,value))*32767));
}
void host_ios_touch_initialize(void) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.name="Halo Touch Controls";
    touch_id=SDL_AttachVirtualJoystick(&desc);
    touch_joystick=SDL_OpenJoystick(touch_id);
    if(!touch_joystick)host_fatal("Could not initialize touch controls: %s",SDL_GetError());
    axis_state(SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-1);
    axis_state(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,-1);
}
int host_ios_gamepads(uint32_t *out,int capacity) {
    int count=0,used=0;SDL_JoystickID *ids=SDL_GetGamepads(&count);
    primary_hardware=0;
    if(capacity>0 && touch_id)out[used++]=touch_id;
    for(int i=0;i<count;i++) {
        if(ids[i]==touch_id)continue;
        if(!primary_hardware) {
            primary_hardware=ids[i];
            if(!SDL_GetGamepadFromID(ids[i]))SDL_OpenGamepad(ids[i]);
        } else if(used<capacity) out[used++]=ids[i];
    }
    SDL_free(ids);return used;
}
int host_ios_gamepad_type(SDL_Gamepad *pad) {
    /* Keep the shared touch/hardware controller in the first recognized port. */
    return SDL_GetGamepadID(pad)==touch_id?SDL_GAMEPAD_TYPE_XBOX360:SDL_GetGamepadType(pad);
}
int host_ios_gamepad_axis(SDL_Gamepad *pad,int axis) {
    int value=SDL_GetGamepadAxis(pad,axis);
    if(SDL_GetGamepadID(pad)==touch_id && primary_hardware) {
        int physical=SDL_GetGamepadAxis(SDL_GetGamepadFromID(primary_hardware),axis);
        if(abs(physical)>abs(value))value=physical;
    }
    return value;
}
int host_ios_gamepad_button(SDL_Gamepad *pad,int button) {
    return SDL_GetGamepadButton(pad,button) ||
        (SDL_GetGamepadID(pad)==touch_id && primary_hardware &&
         SDL_GetGamepadButton(SDL_GetGamepadFromID(primary_hardware),button));
}
void host_ios_touch_reset(void) {
    for(int i=0;i<SDL_GAMEPAD_BUTTON_COUNT;i++)button_state(i,NO);
    for(int i=0;i<SDL_GAMEPAD_AXIS_COUNT;i++)axis_state(i,i>=SDL_GAMEPAD_AXIS_LEFT_TRIGGER?-1:0);
}

@interface HaloButton : UIButton
@property(nonatomic) int gameButton;
@property(nonatomic) int gameAxis;
@end
@implementation HaloButton
- (void)sendDown:(BOOL)down {
    self.highlighted=down;
    if(self.gameAxis>=0)axis_state(self.gameAxis,down?1:-1);
    else button_state(self.gameButton,down);
}
- (BOOL)beginTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {
    [super beginTrackingWithTouch:touch withEvent:event];[self sendDown:YES];return YES;
}
- (void)endTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {
    [super endTrackingWithTouch:touch withEvent:event];[self sendDown:NO];
}
- (void)cancelTrackingWithEvent:(UIEvent *)event {
    [super cancelTrackingWithEvent:event];[self sendDown:NO];
}
- (BOOL)accessibilityActivate {
    [self sendDown:YES];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,150*NSEC_PER_MSEC),dispatch_get_main_queue(),^{[self sendDown:NO];});
    return YES;
}
@end

@interface HaloStick : UIControl
@property(nonatomic) int firstAxis;
@property(nonatomic) CGPoint position;
@property(nonatomic,copy) NSString *caption;
@end
@implementation HaloStick
- (void)updateTouch:(UITouch *)touch {
    CGPoint p=[touch locationInView:self];CGFloat radius=self.bounds.size.width*.34;
    CGFloat x=(p.x-self.bounds.size.width/2)/radius,y=(p.y-self.bounds.size.height/2)/radius;
    CGFloat length=hypot(x,y);if(length>1){x/=length;y/=length;}
    self.position=CGPointMake(x,y);axis_state(self.firstAxis,x);axis_state(self.firstAxis+1,y);
    [self setNeedsDisplay];
}
- (BOOL)beginTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {[self updateTouch:touch];return YES;}
- (BOOL)continueTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {[self updateTouch:touch];return YES;}
- (void)reset {self.position=CGPointZero;axis_state(self.firstAxis,0);axis_state(self.firstAxis+1,0);[self setNeedsDisplay];}
- (void)endTrackingWithTouch:(UITouch *)touch withEvent:(UIEvent *)event {[self reset];}
- (void)cancelTrackingWithEvent:(UIEvent *)event {[self reset];}
- (void)drawRect:(CGRect)rect {
    CGRect circle=CGRectInset(self.bounds,2,2);
    [[UIColor colorWithWhite:0 alpha:.2]setFill];UIBezierPath *path=[UIBezierPath bezierPathWithOvalInRect:circle];[path fill];
    [[UIColor colorWithWhite:1 alpha:.5]setStroke];path.lineWidth=1.5;[path stroke];
    CGFloat size=self.bounds.size.width,r=size*.34;
    CGRect knob=CGRectMake(size/2+self.position.x*r-21,size/2+self.position.y*r-21,42,42);
    [[UIColor colorWithWhite:1 alpha:.25]setFill];[[UIBezierPath bezierPathWithOvalInRect:knob]fill];
    NSDictionary *attrs=@{NSFontAttributeName:[UIFont systemFontOfSize:10 weight:UIFontWeightSemibold],NSForegroundColorAttributeName:[UIColor colorWithWhite:1 alpha:.8]};
    CGSize text=[self.caption sizeWithAttributes:attrs];
    [self.caption drawAtPoint:CGPointMake((size-text.width)/2,size-20) withAttributes:attrs];
}
@end

@interface HaloControls : UIView
@property(nonatomic,strong) HaloStick *moveStick;
@property(nonatomic,strong) HaloStick *lookStick;
@property(nonatomic,strong) NSMutableArray<HaloButton *> *buttons;
@property(nonatomic,strong) UIButton *toggle;
@end
@implementation HaloControls
- (HaloButton *)addButton:(NSString *)title label:(NSString *)label button:(int)button axis:(int)axis {
    HaloButton *b=[HaloButton buttonWithType:UIButtonTypeCustom];b.gameButton=button;b.gameAxis=axis;
    [b setTitle:title forState:UIControlStateNormal];b.titleLabel.font=[UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    b.backgroundColor=[UIColor colorWithWhite:0 alpha:.28];b.layer.borderColor=[UIColor colorWithWhite:1 alpha:.5].CGColor;b.layer.borderWidth=1;
    b.accessibilityLabel=label;b.exclusiveTouch=NO;[self addSubview:b];[self.buttons addObject:b];return b;
}
- (instancetype)initWithFrame:(CGRect)frame {
    if(!(self=[super initWithFrame:frame]))return nil;
    self.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    self.multipleTouchEnabled=YES;self.buttons=[NSMutableArray array];
    self.moveStick=[[HaloStick alloc]init];self.moveStick.firstAxis=SDL_GAMEPAD_AXIS_LEFTX;self.moveStick.caption=@"MOVE";
    self.lookStick=[[HaloStick alloc]init];self.lookStick.firstAxis=SDL_GAMEPAD_AXIS_RIGHTX;self.lookStick.caption=@"LOOK";
    for(HaloStick *s in @[self.moveStick,self.lookStick]){s.backgroundColor=UIColor.clearColor;s.isAccessibilityElement=YES;s.accessibilityLabel=s.caption;[self addSubview:s];}
    [self addButton:@"A" label:@"A — Jump or select" button:SDL_GAMEPAD_BUTTON_SOUTH axis:-1];
    [self addButton:@"B" label:@"B — Melee or back" button:SDL_GAMEPAD_BUTTON_EAST axis:-1];
    [self addButton:@"X" label:@"X — Reload or use" button:SDL_GAMEPAD_BUTTON_WEST axis:-1];
    [self addButton:@"Y" label:@"Y — Switch weapon" button:SDL_GAMEPAD_BUTTON_NORTH axis:-1];
    [self addButton:@"FIRE" label:@"Fire" button:0 axis:SDL_GAMEPAD_AXIS_RIGHT_TRIGGER];
    [self addButton:@"GRENADE" label:@"Throw grenade" button:0 axis:SDL_GAMEPAD_AXIS_LEFT_TRIGGER];
    [self addButton:@"↑" label:@"Menu up" button:SDL_GAMEPAD_BUTTON_DPAD_UP axis:-1];
    [self addButton:@"↓" label:@"Menu down" button:SDL_GAMEPAD_BUTTON_DPAD_DOWN axis:-1];
    [self addButton:@"←" label:@"Menu left" button:SDL_GAMEPAD_BUTTON_DPAD_LEFT axis:-1];
    [self addButton:@"→" label:@"Menu right" button:SDL_GAMEPAD_BUTTON_DPAD_RIGHT axis:-1];
    [self addButton:@"PAUSE" label:@"Pause or start" button:SDL_GAMEPAD_BUTTON_START axis:-1];
    [self addButton:@"CROUCH" label:@"Crouch" button:SDL_GAMEPAD_BUTTON_LEFT_STICK axis:-1];
    [self addButton:@"ZOOM" label:@"Zoom" button:SDL_GAMEPAD_BUTTON_RIGHT_STICK axis:-1];
    [self addButton:@"LIGHT" label:@"Flashlight" button:SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER axis:-1];
    [self addButton:@"SWAP G" label:@"Switch grenade" button:SDL_GAMEPAD_BUTTON_LEFT_SHOULDER axis:-1];
    self.toggle=[UIButton buttonWithType:UIButtonTypeSystem];[self.toggle setTitle:@"Hide controls" forState:UIControlStateNormal];
    [self.toggle setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];self.toggle.titleLabel.font=[UIFont systemFontOfSize:12];
    self.toggle.backgroundColor=[UIColor colorWithWhite:0 alpha:.3];self.toggle.layer.cornerRadius=12;
    [self.toggle addTarget:self action:@selector(toggleControls) forControlEvents:UIControlEventTouchUpInside];[self addSubview:self.toggle];
    [[NSNotificationCenter defaultCenter]addObserver:self selector:@selector(reset) name:UIApplicationWillResignActiveNotification object:nil];
    return self;
}
- (void)reset {host_ios_touch_reset();[self.moveStick reset];[self.lookStick reset];}
- (void)toggleControls {
    [self reset];BOOL hidden=!self.moveStick.hidden;
    self.moveStick.hidden=self.lookStick.hidden=hidden;for(UIView *b in self.buttons)b.hidden=hidden;
    [self.toggle setTitle:hidden?@"Show controls":@"Hide controls" forState:UIControlStateNormal];
}
- (void)layoutSubviews {
    [super layoutSubviews];CGRect r=UIEdgeInsetsInsetRect(self.bounds,self.safeAreaInsets);
    CGFloat left=r.origin.x+16,right=CGRectGetMaxX(r)-16,bottom=CGRectGetMaxY(r)-12,top=r.origin.y+10;
    CGFloat stick=MIN(136,r.size.height*.32),s=44,gap=4;
    self.moveStick.frame=CGRectMake(left,bottom-stick,stick,stick);
    self.lookStick.frame=CGRectMake(right-3*s-20-stick,bottom-stick,stick,stick);
    // Four face buttons in the traditional diamond.
    CGPoint face[4]={{right-2*s-gap,bottom-s},{right-s,bottom-2*s-gap},{right-3*s-2*gap,bottom-2*s-gap},{right-2*s-gap,bottom-3*s-2*gap}};
    for(int i=0;i<4;i++)self.buttons[i].frame=CGRectMake(face[i].x,face[i].y,s,s);
    self.buttons[4].frame=CGRectMake(right-84,top+55,84,48);
    self.buttons[5].frame=CGRectMake(left,top+55,90,48);
    CGFloat d=34,dx=left+stick+12,dy=bottom-3*d;
    self.buttons[6].frame=CGRectMake(dx+d,dy,d,d);
    self.buttons[7].frame=CGRectMake(dx+d,dy+2*d,d,d);
    self.buttons[8].frame=CGRectMake(dx,dy+d,d,d);
    self.buttons[9].frame=CGRectMake(dx+2*d,dy+d,d,d);
    self.buttons[10].frame=CGRectMake(CGRectGetMidX(r)-38,top,76,34);
    self.buttons[11].frame=CGRectMake(left,bottom-stick-42,74,34);
    self.buttons[12].frame=CGRectMake(right-3*s-20-stick,bottom-stick-42,74,34);
    self.buttons[13].frame=CGRectMake(right-180,top+55,80,36);
    self.buttons[14].frame=CGRectMake(left+104,top+55,80,36);
    for(UIButton *b in self.buttons){b.layer.cornerRadius=MIN(b.bounds.size.width,b.bounds.size.height)/2;b.titleLabel.adjustsFontSizeToFitWidth=YES;}
    self.toggle.frame=CGRectMake(right-108,top,108,32);
}
- (BOOL)pointInside:(CGPoint)point withEvent:(UIEvent *)event {
    for(UIView *view in self.subviews)if(!view.hidden && [view pointInside:[self convertPoint:point toView:view] withEvent:event])return YES;
    return NO;
}
@end

void host_ios_touch_attach(SDL_Window *window) {
    UIWindow *native=(__bridge UIWindow *)SDL_GetPointerProperty(SDL_GetWindowProperties(window),SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER,NULL);
    /* Retire SDL's temporary launch window before entering the non-returning
       game loop. Its fade animation can otherwise keep accessibility/input
       attached to the launch controller on recent scene-based iOS versions. */
    for(UIWindow *candidate in native.windowScene.windows) {
        if(candidate!=native && [NSStringFromClass(candidate.rootViewController.class) hasPrefix:@"SDLLaunch"])
            candidate.hidden=YES;
    }
    [native makeKeyAndVisible];
    host_ios_require_landscape(native);
    UIView *root=native.rootViewController.view;
    HaloControls *controls=[[HaloControls alloc]initWithFrame:root.bounds];
    [root addSubview:controls];
    host_logf(HOST_LOG_INFO,"touch controls attached, %.0fx%.0f",root.bounds.size.width,root.bounds.size.height);
}
