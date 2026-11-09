/* SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
typedef unsigned int GLenum, GLuint;
typedef int GLint, GLsizei;
typedef struct { float x,y; } CGPoint;
typedef struct { float width,height; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
extern GLenum glGetError(void);
extern void glGetIntegerv(GLenum,GLint *);
extern void glGetPointerv(GLenum,void **);
extern void glVertexPointer(GLint,GLenum,GLsizei,const void *);
extern void glColorPointer(GLint,GLenum,GLsizei,const void *);
extern void glNormalPointer(GLenum,GLsizei,const void *);
extern void glTexCoordPointer(GLint,GLenum,GLsizei,const void *);
extern void glClientActiveTexture(GLenum);
extern void glEnableClientState(GLenum);
extern void glDisableClientState(GLenum);
extern void glDrawArrays(GLenum,GLint,GLsizei);
extern void glDrawElements(GLenum,GLsizei,GLenum,const void *);
extern void glGenBuffers(GLsizei,GLuint *);
extern void glBindBuffer(GLenum,GLuint);
extern void glBufferData(GLenum,int,const void *,GLenum);
extern void glDeleteBuffers(GLsizei,const GLuint *);
extern void glGenFramebuffersOES(GLsizei,GLuint *);
extern void glBindFramebufferOES(GLenum,GLuint);
extern void glGenRenderbuffersOES(GLsizei,GLuint *);
extern void glDeleteRenderbuffersOES(GLsizei,const GLuint *);
extern void glBindRenderbufferOES(GLenum,GLuint);
extern void glRenderbufferStorageOES(GLenum,GLenum,GLsizei,GLsizei);
extern void glFramebufferRenderbufferOES(GLenum,GLenum,GLenum,GLuint);
extern GLenum glCheckFramebufferStatusOES(GLenum);
extern void glViewport(GLint,GLint,GLsizei,GLsizei);
extern void glClearColor(float,float,float,float);
extern void glColor4f(float,float,float,float);
extern void glClear(GLenum);
extern void glReadPixels(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void *);
__attribute__((objc_root_class)) @interface NSObject { Class isa; }
+ (id)alloc; + (id)new; + (Class)class; - (void)release;
@end
@interface NSAutoreleasePool : NSObject @end
@interface EAGLContext : NSObject
- (id)initWithAPI:(unsigned)api;
+ (signed char)setCurrentContext:(id)context;
- (signed char)renderbufferStorage:(unsigned)target fromDrawable:(id)drawable;
- (signed char)presentRenderbuffer:(unsigned)target;
@end
@interface UIView : NSObject
- (id)initWithFrame:(CGRect)frame; - (id)layer; - (void)addSubview:(id)view;
- (void)setHidden:(signed char)hidden;
@end
@interface UIWindow : UIView
- (void)makeKeyAndVisible;
@end
@interface CAEAGLLayer : NSObject @end
@interface RenderView : UIView @end
@implementation RenderView
+ (Class)layerClass { return [CAEAGLLayer class]; }
@end
enum { VERTEX=0x8074,NORMAL=0x8075,COLOR=0x8076,TEXCOORD=0x8078,
    VERTEX_POINTER=0x808e,TEXCOORD_POINTER=0x8092,ARRAY_BUFFER=0x8892,
    ARRAY_BINDING=0x8894,CLIENT_TEXTURE=0x84e1,TEXTURE0=0x84c0,TEXTURE1=0x84c1,
    FLOAT=0x1406,UNSIGNED_BYTE=0x1401,TRIANGLES=4 };
static unsigned failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n",__LINE__,#x); failures++; } } while (0)
static void red_pixel(void) {
    unsigned char pixel[4]={0};
    glReadPixels(32,32,1,1,0x1908,UNSIGNED_BYTE,pixel);
    CHECK(pixel[0]>200 && pixel[1]<5 && pixel[2]<5 && pixel[3]>200);
    CHECK(glGetError()==0);
}
int main(void) {
    setbuf(stdout,NULL);
    NSAutoreleasePool *pool=[NSAutoreleasePool new];
    EAGLContext *context=[[EAGLContext alloc] initWithAPI:1];
    CHECK(context && [EAGLContext setCurrentContext:context]);
    GLuint framebuffer=0,renderbuffer=0;
    glGenFramebuffersOES(1,&framebuffer); glBindFramebufferOES(0x8d40,framebuffer);
    glGenRenderbuffersOES(1,&renderbuffer); glBindRenderbufferOES(0x8d41,renderbuffer);
    glRenderbufferStorageOES(0x8d41,0x8058,64,64);
    glFramebufferRenderbufferOES(0x8d40,0x8ce0,0x8d41,renderbuffer);
    CHECK(glCheckFramebufferStatusOES(0x8d40)==0x8cd5);
    glViewport(0,0,64,64); glClearColor(0,0,0,1); glColor4f(1,0,0,1);
    const void *unused=(const void *)(uintptr_t)0x60000000;
    glVertexPointer(3,FLOAT,0,unused);
    glNormalPointer(FLOAT,0,unused);
    glColorPointer(4,FLOAT,0,unused);
    glTexCoordPointer(2,FLOAT,0,unused);
    void *returned=NULL; glGetPointerv(VERTEX_POINTER,&returned);
    CHECK(returned==unused && glGetError()==0);
    glGetPointerv(0xffff,&returned);
    CHECK(glGetError()==0x0500 && returned==unused);
    glEnableClientState(VERTEX);
    glDrawArrays(TRIANGLES,0,0);
    glDrawElements(TRIANGLES,0,UNSIGNED_BYTE,unused);
    CHECK(glGetError()==0);
    glDrawArrays(TRIANGLES,0,-1); CHECK(glGetError()==0x0501);
    glDrawArrays(0xffff,0,3); CHECK(glGetError()==0x0500);
    glDrawElements(TRIANGLES,3,FLOAT,unused); CHECK(glGetError()==0x0500);
    CHECK(glGetError()==0);
    float vertices[]={-1,-1,0, 1,-1,0, 0,1,0};
    glVertexPointer(3,FLOAT,0,vertices);
    // Invalid setters must preserve the last valid pointer and its metadata.
    glVertexPointer(1,FLOAT,0,unused);
    CHECK(glGetError()==0x0501);
    glGetPointerv(VERTEX_POINTER,&returned); CHECK(returned==vertices);
    glClear(0x4000); glDrawArrays(TRIANGLES,0,3); red_pixel();
    // Texture-coordinate arrays are per client texture unit.
    float texcoords[]={0,0,1,0,0,1};
    glClientActiveTexture(TEXTURE1);
    glTexCoordPointer(2,FLOAT,0,texcoords); glEnableClientState(TEXCOORD);
    glClientActiveTexture(TEXTURE0);
    glGetPointerv(TEXCOORD_POINTER,&returned); CHECK(returned==unused);
    unsigned char indices[]={0,1,2};
    glClear(0x4000); glDrawElements(TRIANGLES,3,UNSIGNED_BYTE,indices); red_pixel();
    GLint active=0; glGetIntegerv(CLIENT_TEXTURE,&active); CHECK(active==TEXTURE0);
    int32_t fixed_vertices[]={-65536,-65536,0,65536,-65536,0,0,65536,0};
    glVertexPointer(3,0x140c,0,fixed_vertices);
    glClear(0x4000); glDrawArrays(TRIANGLES,0,3); red_pixel();
    glGetPointerv(VERTEX_POINTER,&returned); CHECK(returned==fixed_vertices);
    // Refreshing client arrays cannot change the caller's VBO binding or turn
    // a VBO offset into a guest memory access.
    GLuint buffer=0; glGenBuffers(1,&buffer); glBindBuffer(ARRAY_BUFFER,buffer);
    glBufferData(ARRAY_BUFFER,sizeof(vertices),vertices,0x88e4);
    glVertexPointer(3,FLOAT,0,NULL);
    glClear(0x4000); glDrawArrays(TRIANGLES,0,3); red_pixel();
    GLint binding=0; glGetIntegerv(ARRAY_BINDING,&binding); CHECK(binding==(GLint)buffer);
    glGetPointerv(VERTEX_POINTER,&returned); CHECK(returned==NULL);
    glBindBuffer(ARRAY_BUFFER,0); glVertexPointer(3,FLOAT,0,vertices);
    glDeleteBuffers(1,&buffer);
    EAGLContext *other=[[EAGLContext alloc] initWithAPI:1];
    CHECK([EAGLContext setCurrentContext:other]);
    glGetPointerv(VERTEX_POINTER,&returned); CHECK(returned==NULL);
    CHECK([EAGLContext setCurrentContext:context]);
    glGetPointerv(VERTEX_POINTER,&returned); CHECK(returned==vertices);
    glClear(0x4000); glDrawArrays(TRIANGLES,0,3); red_pixel();
    // A paused fullscreen renderer must survive a covering window even when
    // another EAGLContext is current. Snapshotting cannot alter guest bindings.
    CGRect screen={{0,0},{320,480}};
    UIWindow *base=[[UIWindow alloc] initWithFrame:screen];
    RenderView *view=[[RenderView alloc] initWithFrame:screen];
    [base addSubview:view]; [base makeKeyAndVisible];
    CHECK([context renderbufferStorage:0x8d41 fromDrawable:[view layer]]);
    glViewport(0,0,320,480); glClearColor(1,0,0,1); glClear(0x4000);
    CHECK([context presentRenderbuffer:0x8d41]);
    CHECK([EAGLContext setCurrentContext:other]);
    UIWindow *overlay=[[UIWindow alloc] initWithFrame:screen];
    [overlay makeKeyAndVisible];
    glGetIntegerv(0x8ca7,&binding); CHECK(binding==0);
    CHECK([EAGLContext setCurrentContext:context]);
    glGetIntegerv(0x8ca7,&binding); CHECK(binding==(GLint)renderbuffer);
    glGetIntegerv(0x8ca6,&binding); CHECK(binding==(GLint)framebuffer);
    red_pixel();
    [overlay setHidden:1];
    CHECK([context presentRenderbuffer:0x8d41]);
    glDeleteRenderbuffersOES(1,&renderbuffer);
    [overlay makeKeyAndVisible]; // Deleted buffers must not be captured.
    CHECK(glGetError()==0);
    [overlay release]; [view release]; [base release];
    [EAGLContext setCurrentContext:0]; [other release]; [context release];
    printf("S5LBOX-HLE-GLES-ARRAYS %s failures=%u\n",failures?"FAIL":"PASS",failures);
    [pool release]; return failures?1:0;
}
