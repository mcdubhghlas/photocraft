#include "harness.h"
#include "ui/icons.h"
#include <stdlib.h>
#include <string.h>
static void test_embedded_icons_and_tool_mapping(void)
{
    CHECK(pc_ui_icon_count()==143);
    for(size_t i=0;i<pc_ui_icon_count();i++) {
        const char *name=pc_ui_icon_name(i);CHECK(name && pc_ui_icon_exists(name));
        if(i) { CHECK(strcmp(pc_ui_icon_name(i-1),name)<0); }
        size_t len=0;CHECK(pc_ui_icon_svg(name,&len)!=NULL && len>20);
        unsigned sizes[]={18,27,36};
        for(size_t j=0;j<3;j++) {
            pc_rgba8_image image={0};pc_error err;
            pc_status status=pc_ui_icon_rasterize(name,(uint16_t)sizes[j],&image,&err);
            CHECK(status==PC_OK);if(status!=PC_OK) { PC_T_FAIL("%s: %s",name,err.msg); }
            CHECK(image.width==sizes[j] && image.height==sizes[j]);
            size_t visible=0;
            for(size_t p=0;image.pixels && p<(size_t)image.width*image.height;p++) { visible+=image.pixels[p*4+3]!=0; }
            CHECK(visible>0);free(image.pixels);
        }
    }
    for(size_t i=0;i<PC_TOOL_N;i++) { CHECK(pc_ui_icon_exists(pc_ui_tool_icon(PC_UI_TOOL_ALL[i]))); }
    CHECK(!pc_ui_icon_exists("missing") && !pc_ui_icon_exists(NULL));
    size_t a=0,b=0;CHECK(pc_ui_icon_svg("missing",&a)==pc_ui_icon_svg("square",&b) && a==b);
    CHECK(pc_ui_icon_name(pc_ui_icon_count())==NULL);
    pc_rgba8_image unchanged={1,1,NULL};
    CHECK(pc_ui_icon_rasterize("brush",7,&unchanged,NULL)==PC_ERR_INVALID && unchanged.width==1);
}
int main(int argc,char **argv)
{ RUN(test_embedded_icons_and_tool_mapping);return pc_t_finish(); }
