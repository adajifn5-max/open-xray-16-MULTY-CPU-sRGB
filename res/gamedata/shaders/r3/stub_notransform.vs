#include "common_iostructs.h"
#include "linear_color.h"

//////////////////////////////////////////////////////////////////////////////////////////
// Vertex
v2p_TL main ( v_TL I )
{
	v2p_TL O;

	O.HPos = I.P;
	O.Tex0 = I.Tex0;
	//	Some shaders that use this stub don't need Color at all
	O.Color = vertex_color_linear(I.Color.bgra);	//	swizzle vertex colour

 	return O;
}