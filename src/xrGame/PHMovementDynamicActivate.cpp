
#include "StdAfx.h"

#include "PHMovementControl.h"

// extern	class CPHWorld	*ph_world;
#include "xrPhysics/PHCharacter.h"
#include "xrPhysics/IPhysicsShellHolder.h"
bool CPHMovementControl::ActivateBoxDynamic(
    u32 id, int num_it /*=8*/, int num_steps /*5*/, float resolve_depth /*=0.01f*/)
{
    // Validate before indexing retry history or entering the physics world's activation loop.
    if (!ValidBoxId(id) || num_it <= 0 || num_steps <= 0 || !_valid(resolve_depth) || resolve_depth < 0.f)
    {
        Msg("! [physics.box] rejected ActivateBoxDynamic index=%u count=4 iterations=%d steps=%d depth=%g",
            id, num_it, num_steps, double(resolve_depth));
        return false;
    }
    bool character_exist = CharacterExist();
    if (character_exist && trying_times[id] != u32(-1))
    {
        // Fvector dif;dif.sub(trying_poses[id],cast_fv(dBodyGetPosition(m_character->get_body())));
        Fvector character_body_pos;
        m_character->get_body_position(character_body_pos);
        Fvector dif;
        dif.sub(trying_poses[id], character_body_pos);
        if (Device.dwTimeGlobal - trying_times[id] < 500 && dif.magnitude() < 0.05f)
            return false;
    }
    if (!m_character || m_character->PhysicsRefObject()->ObjectPPhysicsShell())
        return false;
    u32 old_id = BoxID();
    const Fbox previousBox = aabb;

    bool character_disabled = character_exist && !m_character->IsEnabled();

    if (character_exist && id == old_id)
        return true;

    if (!character_exist)
    {
        CreateCharacter();
    }

    Fvector vel;
    Fvector pos;

    GetCharacterVelocity(vel);

    GetCharacterPosition(pos);

    bool ret = ::ActivateBoxDynamic(this, character_exist, id, num_it, num_steps, resolve_depth);

    if (!ret)
    {
        if (!character_exist)
            DestroyCharacter();
        else if (character_disabled)
            m_character->Disable();
        if (ValidBoxId(old_id))
            ActivateBox(old_id);
        else
        {
            // Before the first named box activation, BoxID is u32(-1). Restore the
            // actual previous bounds, not boxes[old_id], after a failed trial.
            aabb.set(previousBox);
            m_dwCurBox = old_id;
            if (m_character && m_character->b_exist)
            {
                dVector3 size = {aabb.x2 - aabb.x1, aabb.y2 - aabb.y1, aabb.z2 - aabb.z1};
                m_character->SetBox(size);
            }
        }
        SetVelocity(vel);
        if (m_character)
            m_character->fix_body_rotation();
        // dBodyID b= !m_character ? 0 : m_character->get_body();//GetBody();
        // if(b)
        //{
        //	dMatrix3 R;
        //	dRSetIdentity (R);
        //	dBodySetAngularVel(b,0.f,0.f,0.f);
        //	dBodySetRotation(b,R);
        //}

        SetPosition(pos);

        // Msg("can not activate!");
    }
    else
    {
        ActivateBox(id);
        // Msg("activate!");
    }

    //	SetOjectContactCallback(saved_callback);
    //	saved_callback=0;
    SetVelocity(vel);

    if (!ret && character_exist)
    {
        trying_times[id] = Device.dwTimeGlobal;

        // trying_poses[id].set(cast_fv(dBodyGetPosition(m_character->get_body())));
        m_character->GetBodyPosition(trying_poses[id]); //.set(cast_fv(dBodyGetPosition(m_character->get_body())));
    }
    else
    {
        trying_times[id] = u32(-1);
    }
    return ret;
}
