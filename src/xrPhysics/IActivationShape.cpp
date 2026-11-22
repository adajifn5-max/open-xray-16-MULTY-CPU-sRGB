#include "StdAfx.h"
#include "IActivationShape.h"
#include "PHActivationShape.h"
#include "Physics.h"
#include "IPhysicsShellHolder.h"
#include "PHCollideValidator.h"
void ActivateShapeExplosive(IPhysicsShellHolder* self_obj, const Fvector& size, Fvector& out_size, Fvector& in_out_pos)
{
    //////////////
    CPHActivationShape activation_shape; // Fvector start_box;m_PhysicMovementControl.Box().getsize(start_box);
    activation_shape.Create(in_out_pos, size, self_obj);

    CPHCollideValidator::SetCharacterClassNotCollide(activation_shape);

    dBodySetGravityMode(activation_shape.ODEBody(), 0);
    activation_shape.Activate(size, 1, 1.f, float(M_PI) / 8.f);
    in_out_pos.set(activation_shape.Position());
    activation_shape.Size(out_size);
    activation_shape.Destroy();
    //////////
}

void ActivateShapePhysShellHolder(
    IPhysicsShellHolder* obj, const Fmatrix& in_xform, const Fvector& in_size, Fvector& in_pos, Fvector& out_pos)
{
    CPHActivationShape activation_shape;
    activation_shape.Create(in_pos, in_size, obj);
    activation_shape.set_rotation(in_xform);
    if (obj->ObjectPPhysicsShell())
    {
        activation_shape.collide_bits() = obj->ObjectPPhysicsShell()->collide_bits();
        activation_shape.collide_class_bits() = obj->ObjectPPhysicsShell()->collide_class_bits();
    }
    activation_shape.Activate(in_size, 1, 1.f, float(M_PI) / 8.f);

    out_pos = activation_shape.Position();
    VERIFY(valid_pos(out_pos, phBoundaries));
    activation_shape.Destroy();

#ifdef DEBUG
    if (!valid_pos(out_pos, phBoundaries))
    {
        Msg("not valid position	%f,%f,%f", out_pos.x, out_pos.y, out_pos.z);
        Msg("size	%f,%f,%f", in_size.x, in_size.y, in_size.z);
        Msg("Object: %s", obj->ObjectName());
        Msg("Visual: %s", obj->ObjectNameVisual());
        // Msg("Object	pos	%f,%f,%f",Position().x,Position().y,Position().z);
    }
#endif // DEBUG
}

bool ActivateShapeCharacterPhysicsSupport(Fvector& out_pos, const Fvector& vbox, const Fvector& activation_pos,
    const Fmatrix& mXFORM, bool not_collide_characters, bool set_rotation, IPhysicsShellHolder* m_EntityAlife,
    u16 correction_attempts)
{
    if (!_valid(activation_pos) || !_valid(vbox) || vbox.x <= 0.f || vbox.y <= 0.f || vbox.z <= 0.f ||
        !correction_attempts)
    {
        out_pos.set(activation_pos);
        Msg("! [spawn.physics] rejected invalid correction input");
        return false;
    }
    CPHActivationShape activation_shape;
    // Resolve a bounded stalker spawn against fixed obstacles instead of pushing nearby bodies.
    // The initial contact query also uses this flag to avoid waking/merging their islands.
    const bool boundedSpawn = correction_attempts == 1 && m_EntityAlife->IsStalker();
    activation_shape.Create(activation_pos, vbox, m_EntityAlife, CPHActivationShape::etBox,
        boundedSpawn ? CPHActivationShape::flStaticEnvironment : 0);
    if (not_collide_characters)
    {
        CPHCollideValidator::SetCharacterClassNotCollide(activation_shape);
    }
    if (set_rotation)
        activation_shape.set_rotation(mXFORM);
    bool ret = activation_shape.Activate(vbox, 1, 1.f, float(M_PI) / 8.f, false, correction_attempts);
    out_pos.set(activation_shape.Position());
    activation_shape.Destroy();
    return ret;
}
