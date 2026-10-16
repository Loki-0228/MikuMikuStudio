struct DefinitionIds { MmsId morph{},rigid{},shape{},joint{}; };
DefinitionIds Definitions(MMDApp* app,const MmsSceneApiV1& api,MmsHandle handle,MmsObjectRef object,bool pmx) {
    auto& model=*mdl::Mdl(app->ModelSlot(0));MmsSpan span{},bones{},vertices{},materials{};
    Check(api.readChannel(api.context,handle,object,MMS_CHANNEL_MORPHS,&span)==MMS_OK && span.count==(pmx?6u:2u),"typed morph definition count");
    const auto* morphs=static_cast<const MmsMorphV1*>(span.data);DefinitionIds saved;saved.morph=morphs[0].id;
    const auto& mesh=Channel<MmsMeshV1>(api,handle,object,MMS_CHANNEL_MESH);
    const auto vertex=static_cast<const MmsVertexV1*>(mesh.vertices.data)->sourceVertex;
    api.readChannel(api.context,handle,object,MMS_CHANNEL_BONES,&bones);
    api.readChannel(api.context,handle,object,MMS_CHANNEL_MATERIALS,&materials);
    Check(morphs[0].kind==MMS_MORPH_VERTEX && morphs[0].elements.stride==sizeof(MmsMorphVertexElementV1),"vertex morph schema and stride");
    const auto& delta=*static_cast<const MmsMorphVertexElementV1*>(morphs[pmx?0:1].elements.data);
    Check(Same(delta.vertex,vertex) && Near(delta.offset[0],1),"vertex morph targets source vertex ID");
    if(pmx) {
        const auto& uv=*static_cast<const MmsMorphUvElementV1*>(morphs[2].elements.data);
        const auto& material=*static_cast<const MmsMorphMaterialElementV1*>(morphs[3].elements.data);
        const auto& bone=*static_cast<const MmsMorphBoneElementV1*>(morphs[4].elements.data);
        const auto& group=*static_cast<const MmsMorphGroupElementV1*>(morphs[5].elements.data);
        Check(morphs[1].kind==MMS_MORPH_UV && morphs[2].kind==MMS_MORPH_UV1 && Same(uv.vertex,vertex) && Near(uv.offset[3],.4f),"UV morph family and four-component delta");
        Check(morphs[3].kind==MMS_MORPH_MATERIAL && material.operation==MMS_MORPH_ADD && Same(material.material,static_cast<const MmsMaterialV1*>(materials.data)->id) && Near(material.toon[3],.1f),"material morph operation, target and tint");
        Check(morphs[4].kind==MMS_MORPH_BONE && Same(bone.bone,static_cast<const MmsBoneV1*>(bones.data)[1].id) && Near(bone.translation[2],3) && Near(bone.rotation[3],1),"bone morph translation and xyzw quaternion");
        Check(morphs[5].kind==MMS_MORPH_GROUP && Same(group.morph,morphs[0].id) && Near(group.weight,.5f),"group morph retains stable child reference");
    } else {
        Check((morphs[0].flags&MMS_MORPH_BASE) && !(morphs[1].flags&MMS_MORPH_BASE),"PMD absolute base is distinguished from a deformation");
        Check(Near(static_cast<const MmsMorphVertexElementV1*>(morphs[0].elements.data)->offset[0],-6),"PMD base stores absolute positions");
    }
    for(size_t i=0;i<span.count;++i)Check(morphs[i].value==0,"bind stage does not expose current morph weight");
    Check(api.readChannel(api.context,handle,object,MMS_CHANNEL_RIGIDS,&span)==MMS_OK && span.count==(pmx?3u:0u),"rigid definition count");
    if(!pmx) {
        Check(api.readChannel(api.context,handle,object,MMS_CHANNEL_JOINTS,&span)==MMS_OK && !span.count,"model with no joints returns an empty implemented channel");
        return saved;
    }
    const auto* rigids=static_cast<const MmsRigidV1*>(span.data);saved.rigid=rigids[0].id;
    Check(api.readChannel(api.context,handle,object,MMS_CHANNEL_SHAPES,&span)==MMS_OK && span.count==3,"sphere, box and capsule shapes");
    const auto* shapes=static_cast<const MmsShapeV1*>(span.data);saved.shape=shapes[0].id;
    for(unsigned i=0;i<3;++i) {
        const auto& rigid=rigids[i];
        Check(rigid.mode==i && shapes[i].kind==i && Same(rigid.shapeId,shapes[i].id) && Same(rigid.boneId,static_cast<const MmsBoneV1*>(bones.data)[i].id),"rigid mode, shape and bone association");
        Check(Near(rigid.world[12],float(i)) && Near(rigid.world[13],float(i+4)) && rigid.linearVelocity[0]==0 && rigid.angularVelocity[0]==0,"rigid rest world and zero bind velocities");
        Check(rigid.group==i && rigid.collisionMask==0xffff && Near(rigid.damping[1],.2f) && Near(rigid.friction,.4f),"rigid physical properties");
        bool sameInverse=true;
        for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col) {
            float product=0;for(unsigned n=0;n<4;++n)product+=rigid.world[row*4+n]*model.rigidTable[i].invTransform[n*4+col];
            sameInverse &= Near(product,row==col?1.f:0.f);
        }
        Check(sameInverse,"ABI rest transform matches native loader inverse");
        bool bindingMatches=true;
        for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col) {
            float product=0;for(unsigned n=0;n<4;++n)product+=rigid.bodyToBone[row*4+n]*rigid.world[n*4+col];
            const float expected=row==3&&col==1?float(i):row==col?1.f:0.f;
            bindingMatches &= Near(product,expected);
        }
        Check(bindingMatches,"body-to-bone reconstructs rest bone world");
    }
    Check(rigids[0].inertia[0]==0 && Near(rigids[1].inertia[0],13.f/6) && rigids[1].inertia[1]==0 && rigids[2].inertia[4]>0,"effective rest inertia for kinematic, box and capsule bodies");
    const auto& joint=Channel<MmsJointV1>(api,handle,object,MMS_CHANNEL_JOINTS);saved.joint=joint.id;
    Check(Same(joint.objectA,object.id) && Same(joint.bodyA,rigids[0].id) && Same(joint.bodyB,rigids[1].id),"joint references both stable rigid IDs");
    Check(joint.kind==MMS_JOINT_SPRING6DOF && Near(joint.linearLower[1],-2) && Near(joint.angularUpper[2],.3f) && Near(joint.springs[5],7) && joint.collideConnected==1 && joint.damping[0]==1,"joint limits, springs and native damping policy");
    btGeneric6DofConstraint* native=nullptr;
    for(int i=0;i<model.scenePtr->world->getNumConstraints();++i) {
        auto* candidate=model.scenePtr->world->getConstraint(i);
        if(candidate->getUserConstraintId()==model.jointTable[0].constraint)native=dynamic_cast<btGeneric6DofConstraint*>(candidate);
    }
    Check(native!=nullptr,"native joint exists for independent comparison");
    float a[16],b[16];native->getFrameOffsetA().getOpenGLMatrix(a);native->getFrameOffsetB().getOpenGLMatrix(b);
    bool matches=true;for(unsigned i=0;i<16;++i)matches &= Near(a[i],joint.frameA[i]) && Near(b[i],joint.frameB[i]);
    Check(matches,"joint body-local frames match native Bullet constraint");
    return saved;
}
