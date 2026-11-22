#include "common_iostructs.h"
#include "linear_color.h"

uniform float4		screen_res;		// Screen resolution (x-Width,y-Height, zw - 1/resolution)

//////////////////////////////////////////////////////////////////////////////////////////
// Vertex
v2p_TL main ( v_TL_positiont I )
{
	v2p_TL O;

//	O.HPos = P;

	{
		I.P.xy += 0.5f;
//		O.HPos.x = I.P.x/1024 * 2 - 1;
//		O.HPos.y = (I.P.y/768 * 2 - 1)*-1;
		O.HPos.x = I.P.x * screen_res.z * 2 - 1;
		O.HPos.y = (I.P.y * screen_res.w * 2 - 1)*-1;
		O.HPos.zw = I.P.zw;
	}

	O.Tex0 = I.Tex0;
	O.Color = vertex_color_linear(float4(I.Color.bgr, 1.0f)) * float4(4, 4, 4, 1);	//	swizzle vertex colour

 	return O;
}