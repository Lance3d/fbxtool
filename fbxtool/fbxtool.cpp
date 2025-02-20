#include "stdafx.h"

#include "fbxtool.h"
#include <cstdio>
#include <stdlib.h>
#include <fbxsdk.h>
#include <map>
#include <list>
#include <iostream>
#include <fstream>
#include <filesystem>

#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "Common.h"
#include "DisplayCommon.h"
#include "GeometryUtility.h"
#include "clara.hpp"
#include "tinydir.h"

using namespace clara;

const std::string physicsSuffix = "__phys";
const std::string ragdollSuffix = "__ragdoll";


struct SJointEnhancement
{
	std::string oldName;
	std::string newName;
	std::string physicsProxy;
	std::string ragdollProxy;
	std::string primitiveType;
	std::string parentNode;
};


std::map<std::string, SJointEnhancement> jointMap;
bool isVerbose { false };
bool applyMixamoFixes { false };
bool addIK { false };
bool gApplyWeaponFix { false };
bool gAddRoot{ false };
std::string gAddRootChildName{ "" };
std::string gAddRootRootName{ "root" };
std::string gRemoveLeafName{ "" };
std::string gAxis{ "" };
double gScale = 1.0;



// Multiply a quaternion by a vector.

FbxVector4 QMulV(const FbxQuaternion& q, const FbxVector4& v)
{
	FbxVector4 out;
	FbxVector4 r2;

	r2.mData [0] = (q.mData [1] * v.mData [2] - q.mData [2] * v.mData [1]) + q.mData [3] * v.mData [0];
	r2.mData [1] = (q.mData [2] * v.mData [0] - q.mData [0] * v.mData [2]) + q.mData [3] * v.mData [1];
	r2.mData [2] = (q.mData [0] * v.mData [1] - q.mData [1] * v.mData [0]) + q.mData [3] * v.mData [2];

	out.mData [0] = (r2.mData [2] * q.mData [1] - r2.mData [1] * q.mData [2]);
	out.mData [0] += out.mData [0] + v.mData [0];
	out.mData [1] = (r2.mData [0] * q.mData [2] - r2.mData [2] * q.mData [0]);
	out.mData [1] += out.mData [1] + v.mData [1];
	out.mData [2] = (r2.mData [1] * q.mData [0] - r2.mData [0] * q.mData [1]);
	out.mData [2] += out.mData [2] + v.mData [2];

	return out;
}

FbxNode* AddNewParent(FbxScene* pFbxScene, FbxNode* pChildNode, const char* parentName, bool addOtherChild = false)
{
	FbxSkeleton* currentBone = pChildNode->GetSkeleton();
    if (!currentBone) return nullptr;

    FbxNode* pCurrentParent = pChildNode->GetParent();
    bool isNewRootNode = (pCurrentParent == nullptr) || (currentBone->GetSkeletonType() == FbxSkeleton::eRoot);

    FbxString newNodeName(parentName);
    FbxSkeleton* skeletonRootAttribute = FbxSkeleton::Create(pFbxScene, parentName);
    skeletonRootAttribute->SetSkeletonType(isNewRootNode ? FbxSkeleton::eRoot : FbxSkeleton::eLimbNode);

    FbxNode* newParentNode = FbxNode::Create(pFbxScene, parentName);
    newParentNode->SetNodeAttribute(skeletonRootAttribute);

	// insert the node as pChildNode and siblings 's new parent
    if (pCurrentParent) {
		if (addOtherChild) {
            std::list<FbxNode*> children;
            for (int i = 0; i < pCurrentParent->GetChildCount(); ++i)
                children.push_back(pCurrentParent->GetChild(i));
            for (auto child : children) {
                pCurrentParent->RemoveChild(child);
                newParentNode->AddChild(child);
            }
            pCurrentParent->AddChild(newParentNode);
		}
		else {
			pCurrentParent->RemoveChild(pChildNode);
			pCurrentParent->AddChild(newParentNode);
			newParentNode->AddChild(pChildNode);
		}
    }
	else {
		newParentNode->AddChild(pChildNode);
	}
    if (isNewRootNode) currentBone->SetSkeletonType(FbxSkeleton::eLimbNode);

    return newParentNode;
}

bool RemoveNode(FbxScene* pFbxScene, FbxNode* pNode)
{
	FbxNode* pCurrentParent = pNode->GetParent();
	if (!pCurrentParent) {
		FBXSDK_printf("Can not remove root node %s\n", pNode->GetName());
		return false;
	}

	pCurrentParent->RemoveChild(pNode);

	std::list<FbxNode*> children;
	for (int i = 0; i < pNode->GetChildCount(); ++i)
		children.push_back(pNode->GetChild(i));
	for (auto child : children) {
		pNode->RemoveChild(child);
		pCurrentParent->AddChild(child);
	}

	return pFbxScene->RemoveNode(pNode);
}

void ResetBoneTransform(FbxNode* node)
{
	FbxSkeleton* bone = node->GetSkeleton();	

	if (bone) {
		std::cout << "Bone: " << node->GetName();
		node->SetName("root");
		std::cout << " renamed to: " << node->GetName() << std::endl;		
	}

    FbxMesh* mesh = node->GetMesh();
	if (mesh && mesh->GetDeformerCount(FbxDeformer::eSkin) > 0) {
        FbxDeformer* def = mesh->GetDeformer(0, FbxDeformer::eSkin);        
        FbxSkin* skin = FbxCast<FbxSkin>(def);
		if (skin) {
            int clusterCount = skin->GetClusterCount();
            for (int i = 0; i < clusterCount; i++) {
                FbxCluster* cluster = skin->GetCluster(i);
                //FbxCluster::ELinkMode lClusterMode = cluster->GetLinkMode();
                //const char* boneName = cluster->GetLink()->GetName();

                FbxAMatrix kLinkMatrix;
                cluster->GetTransformLinkMatrix(kLinkMatrix);
                //FbxAMatrix kTM;
                //cluster->GetTransformMatrix(kTM);                

                FbxAMatrix kInvLinkMatrix(kLinkMatrix.Inverse());
                //FbxAMatrix kM(kInvLinkMatrix * kTM);                
                
				// reset deformer transform and put mesh to origin
                cluster->SetTransformLinkMatrix(FbxAMatrix());                
				node->SetPivotState(FbxNode::eDestinationPivot, FbxNode::ePivotActive);
                node->SetGeometricTranslation(FbxNode::eDestinationPivot, -kInvLinkMatrix.GetT() * 0.5);
                //node->LclTranslation.Set(-kLinkMatrix.GetT());


                //invBindPose[boneName] = kM;		
                //bindPose[boneName] = kM.Inverse();		
                //int indexCount = cluster->GetControlPointIndicesCount();		
                //int* indices = cluster->GetControlPointIndices();		
                //double* weights = cluster->GetControlPointWeights();		
                //int bone = boneMap[boneName];		
                //for (int j = 0; j < indexCount; j++) { 
                //	int vertex = indices[j];			
                //	float weight = (float)weights[j];			
                //	BoneWeightPair pair;							
                //	pair.bone = bone;			
                //	pair.weight = weight;			
                //	boneWeights[vertex].push_back(pair); 
                //} 
            }
		}
	}	

	for (int i = 0; i < node->GetChildCount(); ++i) {
		ResetBoneTransform(node->GetChild(i));
	}
}

void ApplyWeaponFix(FbxScene* pFbxScene)
{
	FbxNode* sceneRoot = pFbxScene->GetRootNode();	
	ResetBoneTransform(sceneRoot);

    //if (pFbxScene->GetPoseCount() > 0) {
    //    auto pose = pFbxScene->GetPose(0);
    //    int poseNodeCount = pose->GetCount();
    //    std::cout << "Poses begin" << std::endl;
    //    for (int i = 0; i < poseNodeCount; ++i) {
    //        auto kNode = pose->GetNode(i);
    //        if (kNode) {
    //            //kNode->SetPivotState(FbxNode::eSourcePivot, FbxNode::ePivotActive);
    //            //kNode->SetPivotState(FbxNode::eDestinationPivot, FbxNode::ePivotActive);
    //            std::cout << kNode->GetName() << std::endl;
    //            Display3DVector("lcl trans:", kNode->LclTranslation.Get(), "\n");
    //            //kNode->SetGeometricTranslation(FbxNode::eDestinationPivot, FbxVector4());
    //            //kNode->LclTranslation.Set(FbxVector4());
    //            Display3DVector("lcl trans:", kNode->LclTranslation.Get(), "\n");
    //        }
    //    }
    //    std::cout << "Poses end" << std::endl;
    //}

	sceneRoot->ConvertPivotAnimationRecursive(pFbxScene->GetCurrentAnimationStack(), FbxNode::eDestinationPivot, 30.0);
}

void RenameSkeleton(FbxScene* pFbxScene, FbxNode* pFbxNode, std::string indexName, std::map<std::string, SJointEnhancement> jointMap)
{
	FbxSkeleton* lSkeleton = (FbxSkeleton*)pFbxNode->GetNodeAttribute();

	// See if we have a new name for the joint.
	if (jointMap.find(indexName) != jointMap.end())
	{
		FbxString stringName = jointMap [indexName].newName.c_str();

		if (isVerbose)
			DisplayString("NEW NAME: " + stringName);
		pFbxNode->SetName(stringName);
	}
	else
	{
		DisplayString("Name: ", pFbxNode->GetName());
	}

	DisplayInt("  Skeleton Type: ", lSkeleton->GetSkeletonType());

	auto transform = pFbxNode->LclTranslation.Get();
	Display3DVector("  Transform: ", transform);
	auto rotation = pFbxNode->LclRotation.Get();
	Display3DVector("  Rotation: ", rotation);

	auto preRotation = pFbxNode->PreRotation.Get();
	Display3DVector("  Pre-Rotation: ", preRotation);
	auto postRotation = pFbxNode->PostRotation.Get();
	Display3DVector("  Post-Rotation: ", postRotation);

	//FbxAMatrix& worldTM = pFbxNode->EvaluateGlobalTransform();
	//Display3DVector("  World Rotation: ", worldTM.GetR());
	//Display3DVector("  World Translation: ", worldTM.GetT());

	if (strcmp(pFbxNode->GetName(), "Hips") == 0)
	{
		// Zero the hips, Mixamo leaves them offset slightly.
		transform.mData [0] = 0.0f;
		transform.mData [2] = 0.0f;
		pFbxNode->LclTranslation.Set(transform);

		// Zero the pre-rotation.
		//FbxDouble3 preRotation(0.0f, 0.0f, 0.0f);
		//pFbxNode->PreRotation.Set(preRotation);

		// Apply a rotation.
		//double temp = preRotation.mData [1];
		//preRotation.mData [1] = preRotation.mData [2];
		//preRotation.mData [2] = -temp;
		//pFbxNode->PreRotation.Set(preRotation);
	}

	DisplayString("");
}


void EnhanceSkeleton(FbxScene* pFbxScene, FbxNode* pFbxNode, std::string indexName, std::map<std::string, SJointEnhancement> jointMap)
{
	FbxSkeleton* pFBXSkeleton = (FbxSkeleton*)pFbxNode->GetNodeAttribute();

	// Check to see if there is any enhancement data in the JSON file for this joint.
	if (jointMap.find(indexName) != jointMap.end())
	{
		//SJointEnhancement joint = jointMap [indexName];

		//// Physics, yo!
		//if (!joint.physicsProxy.empty())
		//{
		//	FBXSDK_printf("PHYSICS for %s\n", joint.newName.c_str());
		//	auto physName = joint.newName + physicsSuffix;
		//	FbxDouble3 trans { 0.0f, 0.0f, 0.0f };
		//	auto newNode = CreateCube(pFbxScene, physName.c_str(), trans);
		//	
		//	// Create a mesh object
		//	//FbxMesh* pMesh = FbxMesh::Create(pFbxScene, physName.c_str());

		//	// Add it to the scene.
		//	if (auto pFbxNode = pFbxScene->FindNodeByName(joint.newName.c_str()))
		//	{
		//		pFbxNode->AddChild(newNode);

		//		// Set the mesh as the node attribute of the node
		//		//pFbxNode->SetNodeAttribute(newNode);
		//	}
		//}

		// Ragdoll.
		//if (!joint.ragdollProxy.empty())
		//{
		//	FBXSDK_printf("RAGDOLL for %s\n", joint.newName.c_str());
		//	auto physName = joint.newName + ragdollSuffix;
		//	auto transform = pFbxNode->LclTranslation.Get();
		//	auto newNode = CreateCube(pFbxScene, physName.c_str(), transform);

		//	newNode->LclRotation.Set(pFbxNode->LclRotation.Get());
		//	newNode->PreRotation.Set(pFbxNode->PreRotation.Get());
		//	newNode->PostRotation.Set(pFbxNode->PostRotation.Get());

		//	// Add it to the scene.
		//	if (auto pFbxNode = pFbxScene->FindNodeByName(joint.parentNode.c_str()))
		//	{
		//		pFbxNode->AddChild(newNode);
		//	}
		//	else
		//	{
		//		FbxNode* sceneRootNode = pFbxScene->GetRootNode();

		//		if (sceneRootNode)
		//		{
		//			sceneRootNode->AddChild(newNode);
		//		}
		//	}
		//}
	}
}


void DoSkeletonStuff(FbxScene* pFbxScene, FbxNode* pFbxNode, std::map<std::string, SJointEnhancement> jointMap)
{
	if (jointMap.find(std::string(pFbxNode->GetName())) != jointMap.end())
	{
		// We're renaming the skeleton on the fly, so it's important to remember what the node 'was' called and use that for all lookups in the map.
		auto indexName = jointMap [std::string(pFbxNode->GetName())].oldName;

		// Rename to new skeleton standard.
		RenameSkeleton(pFbxScene, pFbxNode, indexName, jointMap);
		EnhanceSkeleton(pFbxScene, pFbxNode, indexName, jointMap);
	}
}


void InterateContent(FbxScene* pFbxScene, FbxNode* pFbxNode)
{
	FbxNodeAttribute::EType attributeType;

	if (pFbxNode->GetNodeAttribute() == nullptr)
	{
		FBXSDK_printf("NULL Node Attribute\n\n");
	}
	else
	{
		attributeType = (pFbxNode->GetNodeAttribute()->GetAttributeType());

		switch (attributeType)
		{
			case FbxNodeAttribute::eSkeleton:
				DoSkeletonStuff(pFbxScene, pFbxNode, jointMap);
				break;

			default:
				break;
		}
	}

	for (int i = 0; i < pFbxNode->GetChildCount(); i++)
	{
		InterateContent(pFbxScene, pFbxNode->GetChild(i));
	}
}

void RemoveLeafBones(FbxScene* pFbxScene, FbxNode* pFbxNode)
{
    for (int i = pFbxNode->GetChildCount() - 1; i >= 0; --i) {
        RemoveLeafBones(pFbxScene, pFbxNode->GetChild(i));
    }
	
	std::string boneName = pFbxNode->GetName();
    if (boneName.find(gRemoveLeafName) != std::string::npos) {
        if (pFbxNode->GetChildCount() == 0) {
            bool b = RemoveNode(pFbxScene, pFbxNode);
            std::cout << boneName << " removed:" << b << std::endl;
        }
        else {
            std::cout << boneName << " is not leaf, can not remove!" << std::endl;
        }
    }
}

typedef std::map<std::string, FbxAMatrix> BoneGlobalTransform;
BoneGlobalTransform _gBoneGlobalTransforms;

// scale bone's local translate
void ScaleNodeRecursive(FbxScene* pFbxScene, FbxNode* pNode)
{
	FbxNode* pParent = pNode->GetParent();
	FbxAMatrix parentGlobal;
	if (pParent)
		parentGlobal = _gBoneGlobalTransforms[pParent->GetName()];
	
	FbxAMatrix& globalMat = _gBoneGlobalTransforms[pNode->GetName()];
	FbxVector4 globalT = globalMat.GetT();

	parentGlobal.SetS(FbxDouble3(1, 1, 1));
	FbxAMatrix parentGlobalInv = parentGlobal.Inverse();	
	FbxVector4 newLocalT = parentGlobalInv.MultT(globalT);	
	pNode->LclTranslation.Set(newLocalT);

	for (int i = 0; i < pNode->GetChildCount(); ++i) {
		ScaleNodeRecursive(pFbxScene, pNode->GetChild(i));
	}
}

void ScaleMeshes(FbxNode* pNode)
{	
    FbxMesh* pMesh = pNode->GetMesh();	

    if (pMesh) {		
		// scale the mesh vertex
        FbxVector4* vertices = pMesh->GetControlPoints();
		for (int i = 0; i < pMesh->GetControlPointsCount(); ++i) {
			vertices[i] *= gScale;
		}

		// scale the cluster(bone) if any
		int skinCount = pMesh->GetDeformerCount(FbxDeformer::eSkin);
		for (int i = 0; i < skinCount; ++i) {
			FbxSkin* pSkin = (FbxSkin*)pMesh->GetDeformer(i, FbxDeformer::eSkin);
			if(!pSkin) continue;

			int clusterCount = pSkin->GetClusterCount();
			for (int j = 0; j < clusterCount; ++j) {
				FbxCluster* pCluster = pSkin->GetCluster(j);
				const char* clusterName = pCluster->GetLink()->GetName();
				pCluster->SetTransformLinkMatrix(_gBoneGlobalTransforms[clusterName]);
			}
		}
    }

    for (int i = 0; i < pNode->GetChildCount(); ++i) {
		ScaleMeshes(pNode->GetChild(i));
    }
}

void GetBoneGlobalTransRecursive(FbxNode* pNode, BoneGlobalTransform& boneGlobalTransforms)
{
	FbxAMatrix& globalMat = pNode->EvaluateGlobalTransform(FBXSDK_TIME_INFINITE, FbxNode::eSourcePivot, false, true);
	boneGlobalTransforms[pNode->GetName()] = globalMat;

    for (int i = 0; i < pNode->GetChildCount(); ++i) {
		GetBoneGlobalTransRecursive(pNode->GetChild(i), boneGlobalTransforms);
    }
}

void ScaleScene(FbxScene* pFbxScene)
{
	// delete animation data because we do not want to handle it
    FbxAnimStack* animStack = pFbxScene->GetCurrentAnimationStack();
    if (animStack) pFbxScene->RemoveAnimStack(animStack->GetName());

    FbxNode* sceneRootNode = pFbxScene->GetRootNode();
    sceneRootNode->LclScaling.Set(FbxDouble3(gScale, gScale, gScale));
    //sceneRootNode->ResetPivotSetAndConvertAnimation();

    GetBoneGlobalTransRecursive(sceneRootNode, _gBoneGlobalTransforms);
    ScaleNodeRecursive(pFbxScene, sceneRootNode);

    sceneRootNode->LclScaling.Set(FbxDouble3(1, 1, 1));
    //sceneRootNode->ResetPivotSetAndConvertAnimation();

    GetBoneGlobalTransRecursive(sceneRootNode, _gBoneGlobalTransforms);
    ScaleMeshes(sceneRootNode);
}

void InterateContent(FbxScene* pFbxScene)
{
	FbxNode* sceneRootNode = pFbxScene->GetRootNode();

	if (!gRemoveLeafName.empty()) {
		RemoveLeafBones(pFbxScene, sceneRootNode);
	}

	if (gAddRoot) {
        // apply bone hierarchy fix (add a new root node)
        FbxNode* sklRoot = sceneRootNode->FindChild(gAddRootChildName.c_str(), true, false);
        if (sklRoot) {
            FbxNode* newRoot = AddNewParent(pFbxScene, sklRoot, gAddRootRootName.c_str());
        }
	}
	
	if(gApplyWeaponFix)
		ApplyWeaponFix(pFbxScene);

	if (sceneRootNode)
	{
		for (int i = 0; i < sceneRootNode->GetChildCount(); i++)
		{
			InterateContent(pFbxScene, sceneRootNode->GetChild(i));
		}
	}
    
	if(abs(gScale - 1.0) > DBL_EPSILON)
		ScaleScene(pFbxScene);
}


void DisplayMetaData(FbxScene* pFbxScene)
{
	FbxDocumentInfo* sceneInfo = pFbxScene->GetSceneInfo();

	if (sceneInfo)
	{
		FBXSDK_printf("\n\n--------------------\nMeta-Data\n--------------------\n\n");
		FBXSDK_printf("    Title: %s\n", sceneInfo->mTitle.Buffer());
		FBXSDK_printf("    Subject: %s\n", sceneInfo->mSubject.Buffer());
		FBXSDK_printf("    Author: %s\n", sceneInfo->mAuthor.Buffer());
		FBXSDK_printf("    Keywords: %s\n", sceneInfo->mKeywords.Buffer());
		FBXSDK_printf("    Revision: %s\n", sceneInfo->mRevision.Buffer());
		FBXSDK_printf("    Comment: %s\n", sceneInfo->mComment.Buffer());

		FbxThumbnail* thumbnail = sceneInfo->GetSceneThumbnail();
		if (thumbnail)
		{
			FBXSDK_printf("    Thumbnail:\n");

			switch (thumbnail->GetDataFormat())
			{
				case FbxThumbnail::eRGB_24:
					FBXSDK_printf("        Format: RGB\n");
					break;
				case FbxThumbnail::eRGBA_32:
					FBXSDK_printf("        Format: RGBA\n");
					break;
			}

			switch (thumbnail->GetSize())
			{
				default:
					break;
				case FbxThumbnail::eNotSet:
					FBXSDK_printf("        Size: no dimensions specified (%ld bytes)\n", thumbnail->GetSizeInBytes());
					break;
				case FbxThumbnail::e64x64:
					FBXSDK_printf("        Size: 64 x 64 pixels (%ld bytes)\n", thumbnail->GetSizeInBytes());
					break;
				case FbxThumbnail::e128x128:
					FBXSDK_printf("        Size: 128 x 128 pixels (%ld bytes)\n", thumbnail->GetSizeInBytes());
			}
		}
	}
}


/**
Rename the first animation in the set. Use case: you have a bunch of single animation FBX files, but the internal
animations have names like 'Take 001' or 'Unreal Take'. This will rename only the first animation, but it will also
emit the names of any other animations as debug.

\param [in,out]	pFbxScene  If non-null, the scene.
\param 		   	newName The new name for the first animation take.
**/
void RenameFirstAnimation(FbxScene* pFbxScene, const char* newName)
{
	for (int i = 0; i < pFbxScene->GetSrcObjectCount<FbxAnimStack>(); i++)
	{
		FbxAnimStack* pFbxAnimStack = pFbxScene->GetSrcObject<FbxAnimStack>(i);

		FbxString lOutputString = "Animation Stack Name: ";
		lOutputString += "\nRenamed from ";
		lOutputString += pFbxAnimStack->GetName();
		lOutputString += " to ";
		lOutputString += newName;
		lOutputString += "\n\n";
		FBXSDK_printf(lOutputString);

		// Rename only the first animation.
		if (i == 0)
			pFbxAnimStack->SetName(newName);
	}
}


void ApplyMixamoFixes(FbxManager* pFbxManager, FbxScene* pFbxScene)
{
	FbxNode* sceneRootNode = pFbxScene->GetRootNode();

	// Fix stupid names.
	if (auto pFbxNode = pFbxScene->FindNodeByName("default"))
		pFbxNode->SetName("Eyes");
	if (auto pFbxNode = pFbxScene->FindNodeByName("Tops"))
		pFbxNode->SetName("Top");
	if (auto pFbxNode = pFbxScene->FindNodeByName("Bottoms"))
		pFbxNode->SetName("Bottom");

	// Add a new node and move the meshes onto it.
	FbxNode* meshNode = FbxNode::Create(pFbxScene, "Meshes");
	sceneRootNode->AddChild(meshNode);
	if (auto pFbxNode = pFbxScene->FindNodeByName("Body"))
		meshNode->AddChild(pFbxNode);
	if (auto pFbxNode = pFbxScene->FindNodeByName("Bottom"))
		meshNode->AddChild(pFbxNode);
	if (auto pFbxNode = pFbxScene->FindNodeByName("Eyes"))
		meshNode->AddChild(pFbxNode);
	if (auto pFbxNode = pFbxScene->FindNodeByName("Eyelashes"))
		meshNode->AddChild(pFbxNode);
	if (auto pFbxNode = pFbxScene->FindNodeByName("Hair"))
		meshNode->AddChild(pFbxNode);
	if (auto pFbxNode = pFbxScene->FindNodeByName("Top"))
		meshNode->AddChild(pFbxNode);
	if (auto pFbxNode = pFbxScene->FindNodeByName("Shoes"))
		meshNode->AddChild(pFbxNode);

	// We also need a 'RootProxy' for scaling at some point.
	FbxString rootProxyName("RootProxy");
	FbxNode* skeletonRootProxyNode = FbxNode::Create(pFbxScene, rootProxyName.Buffer());
	sceneRootNode->AddChild(skeletonRootProxyNode);

	// Add a new node called 'Root' to parent the existing skeleton onto.
	FbxString rootName("Root");
	FbxSkeleton* skeletonRootAttribute = FbxSkeleton::Create(pFbxScene, rootName);
	skeletonRootAttribute->SetSkeletonType(FbxSkeleton::eRoot);
	FbxNode* skeletonRootNode = FbxNode::Create(pFbxScene, rootName.Buffer());
	skeletonRootNode->SetNodeAttribute(skeletonRootAttribute);
	skeletonRootProxyNode->AddChild(skeletonRootNode);

	// Reparent the hips.
	if (auto pFbxNode = pFbxScene->FindNodeByName("Hips"))
	{
		skeletonRootNode->AddChild(pFbxNode);
	}
}


void AddNewJoint(FbxManager* pFbxManager, FbxScene* pFbxScene, const char* nodeName, const char* parentNodeName,
	int offsetType = 0,
	FbxVector4 offset = FbxVector4(0.0f, 0.0f, 0.0f),
	FbxQuaternion rotation = FbxQuaternion()
)
{
	FbxNode* sceneRootNode = pFbxScene->GetRootNode();

	// Add a new node  and parent it.
	FbxString newNodeName(nodeName);
	FbxSkeleton* skeletonRootAttribute = FbxSkeleton::Create(pFbxScene, newNodeName);
	skeletonRootAttribute->SetSkeletonType(FbxSkeleton::eLimbNode);

	FbxNode* skeletonNode = FbxNode::Create(pFbxScene, newNodeName.Buffer());
	skeletonNode->SetNodeAttribute(skeletonRootAttribute);

	// The default offset is fine in most cases.
	skeletonNode->LclTranslation.Set(offset);

	// Parent the node.
	if (auto pParentNode = pFbxScene->FindNodeByName(parentNodeName))
	{
		// Foot plane weights.
		if (offsetType == 1)
		{
			// These are meant to point upwards and be approximately 100 units in length. Taking the inverse of the world
			// rotation and multiplying it by an up vector will give us a second vector pointing in the right direction. 
			FbxAMatrix& worldTM = pParentNode->EvaluateGlobalTransform();
			FbxQuaternion rot = worldTM.GetQ();
			rot.Inverse();
			FbxVector4 newVector = QMulV(rot, FbxVector4 { 0.0f, 1.0f, 0.0f });
			skeletonNode->LclTranslation.Set(newVector * 100.0f);
		}

		// Foot target - should be at 0 on the Z axis.
		if (offsetType == 2)
		{
			// These are meant to point straight down and be level with the ground plane. Taking the inverse of the world
			// rotation and multiplying it by an down vector will give us a second vector pointing in the right direction. Just
			// multiply that by the world height of the foot and you're set. 
			FbxAMatrix& worldTM = pParentNode->EvaluateGlobalTransform();
			FbxQuaternion rot = worldTM.GetQ();
			rot.Inverse();
			FbxVector4 trans = worldTM.GetT();
			double length = trans.mData [1];
			FbxVector4 newVector = QMulV(rot, FbxVector4 { 0.0f, -1.0f, 0.0f });
			skeletonNode->LclTranslation.Set(newVector * length);
		}

		// Add this to it's proper parent.
		pParentNode->AddChild(skeletonNode);
	}
}


void AddIkJoints(FbxManager* pFbxManager, FbxScene* pFbxScene)
{
	// Foot planting.
	AddNewJoint(pFbxManager, pFbxScene, "RightFootIKTarget", "RightFoot", 2, FbxVector4(0.0f, 0.0f, 0.0f));
	AddNewJoint(pFbxManager, pFbxScene, "RightFootIKWeight", "RightFoot", 1, FbxVector4(0.0f, 100.0f, 0.0f));
	AddNewJoint(pFbxManager, pFbxScene, "LeftFootIKTarget", "LeftFoot", 2, FbxVector4(0.0f, 0.0f, 0.0f));
	AddNewJoint(pFbxManager, pFbxScene, "LeftFootIKWeight", "LeftFoot", 1, FbxVector4(0.0f, 100.0f, 0.0f));

	// Hands - weapon bones and positioning IK.
	AddNewJoint(pFbxManager, pFbxScene, "RightHandIK", "RightHand", 0, FbxVector4(0.0f, 20.0f, 0.0f));
	AddNewJoint(pFbxManager, pFbxScene, "LeftHandIK", "LeftHand", 0, FbxVector4(0.0f, 20.0f, 0.0f));

	// Looking.
	AddNewJoint(pFbxManager, pFbxScene, "HeadIKLook", "Head", 0, FbxVector4(0.0f, 0.0f, 6.0f));

	// Camera. Just place it approximately where it typically goes for now.
	AddNewJoint(pFbxManager, pFbxScene, "Camera", "Head", 0, FbxVector4(0.0f, 8.3f, 7.4f));
}


bool ProcessFile(FbxManager* pFbxManager, FbxScene* pFbxScene, FbxString fbxInFilePath, FbxString fbxOutFilePath)
{
	bool result = false;

	// Load the scene if there is one.
	if (!fbxInFilePath.IsEmpty())
	{
		FBXSDK_printf("\n\nFile: %s\n\n", fbxInFilePath.Buffer());

		if (LoadScene(pFbxManager, pFbxScene, fbxInFilePath))
		{
			// switch Axis
			if (!gAxis.empty()) {
                FbxAxisSystem axis;
                FbxAxisSystem::ParseAxisSystem(gAxis.c_str(), axis);
                axis.DeepConvertScene(pFbxScene);
			}

			// Display the scene.
			DisplayMetaData(pFbxScene);
			InterateContent(pFbxScene);

			if (applyMixamoFixes)
				ApplyMixamoFixes(pFbxManager, pFbxScene);

			// Add a set of joints for IK management.
			if (addIK)
				AddIkJoints(pFbxManager, pFbxScene);

			// We really only want the base part of the filename. This code is windows specific and MS compiler specific.
			char fname [255];
			char ext [20];
			_splitpath_s(fbxInFilePath, nullptr, 0, nullptr, 0, fname, sizeof(fname), ext, sizeof(ext));

			// Optionally, rename the animation to the filename.
			RenameFirstAnimation(pFbxScene, fname);

			// Save a copy of the scene to a new file.
			result = SaveScene(pFbxManager, pFbxScene, fbxOutFilePath);

			if (result == false)
				FBXSDK_printf("\n\nAn error occurred while saving the scene...\n");
		}
		else
		{
			FBXSDK_printf("\n\nAn error occurred while loading the scene...");
		}
	}
	else
	{
		FBXSDK_printf("\n\nUsage: ImportScene <FBX file name>\n\n");
	}

	return result;
}

FbxString StdStr2FbxStr(std::string str)
{
    FbxString retStr = "";

    char* newStr = NULL;
    FbxAnsiToUTF8(str.c_str(), newStr);//Character encoding conversion API provided by Fbx Sdk
    if (NULL != newStr)
    {
        retStr = newStr;
        delete[] newStr;//remember to release
    }

    return retStr;
}

FbxString WStr2FbxStr(std::wstring str)
{
    FbxString retStr = "";

    char* newStr = NULL;
    FbxWCToUTF8(str.c_str(), newStr);//Character encoding conversion API provided by Fbx Sdk
    if (NULL != newStr)
    {
        retStr = newStr;
        delete[] newStr;//remember to release
    }

    return retStr;
}

void ProcessDirectory(FbxManager* pFbxManager, FbxScene* pFbxScene, std::wstring inputRootPath, std::wstring outputRootPath, std::wstring currentPath)
{
	bool bAbort = false;
    tinydir_dir dir;
    if (tinydir_open(&dir, currentPath.c_str()) != -1)
    {
        while ((dir.has_next) && (!bAbort))
        {
            tinydir_file file;
            if (tinydir_readfile(&dir, &file) != -1)
            {				
				if ((file.is_dir > 0) && lstrcmpiW(file.name, L".") && lstrcmpiW(file.name, L".."))
				{
					ProcessDirectory(pFbxManager, pFbxScene, inputRootPath, outputRootPath, file.path);
				}				
				else if (lstrcmpiW(file.extension, L"FBX") == 0)
                {
                    std::wstring outputFilePath = file.path;
                    outputFilePath.replace(0, inputRootPath.size(), outputRootPath);
					
					std::filesystem::path oPath(outputFilePath);
					oPath.remove_filename();
					std::filesystem::create_directories(oPath);
                    ProcessFile(pFbxManager, pFbxScene, WStr2FbxStr(file.path), WStr2FbxStr(outputFilePath));
                }

                if (tinydir_next(&dir) == -1)
                {
                    FBXSDK_printf("Error getting next file");
                    bAbort = true;
                }
            }
            else
            {
                FBXSDK_printf("Error getting file");
                bAbort = true;
            }
        }
    }
    else
    {
        FBXSDK_printf("Folder not found.");
    }

    tinydir_close(&dir);    
}

int ReadJointFile(std::string &jointMetaFilePath)
{
	// Read joints file.
	std::ifstream jointStream(jointMetaFilePath);
	std::stringstream lineStream;
	if (jointStream)
	{
		lineStream << jointStream.rdbuf();
		jointStream.close();
	}
	std::string jointString = lineStream.str();

	rapidjson::Document jointJSONDocument;

	if (jointJSONDocument.Parse(jointString.c_str()).HasParseError())
	{
		auto error = jointJSONDocument.GetParseError();
		std::cerr << "JSON Read Error: " << error << std::endl;
		return 1;
	}
	else
	{
		if (jointJSONDocument.HasMember("joints") && jointJSONDocument["joints"].IsArray()) {
            const rapidjson::Value& joints = jointJSONDocument["joints"];
            for (rapidjson::SizeType i = 0; i < joints.Size(); i++)
            {
                assert(joints[i].IsObject());

                SJointEnhancement newJoint;

                // Load up our map with the data in the JSON file.
                newJoint.oldName = joints[i]["old-name"].GetString();
                newJoint.newName = joints[i]["new-name"].GetString();

                if (joints[i].HasMember("physics-proxy"))
                {
                    newJoint.physicsProxy = joints[i]["physics-proxy"].GetString();
                }

                if (joints[i].HasMember("ragdoll-proxy"))
                {
                    newJoint.ragdollProxy = joints[i]["ragdoll-proxy"].GetString();
                }

                if (joints[i].HasMember("primitive-type"))
                {
                    newJoint.primitiveType = joints[i]["primitive-type"].GetString();
                }

                if (joints[i].HasMember("parent-node"))
                {
                    newJoint.parentNode = joints[i]["parent-node"].GetString();
                }

                jointMap[newJoint.oldName] = newJoint;
            }
		}

		if(jointJSONDocument.HasMember("axis") && jointJSONDocument["axis"].IsString())
			gAxis = jointJSONDocument["axis"].GetString();
		if (jointJSONDocument.HasMember("applyWeaponFix") && jointJSONDocument["applyWeaponFix"].IsBool())
			gApplyWeaponFix = jointJSONDocument["applyWeaponFix"].GetBool();
		if (jointJSONDocument.HasMember("addRoot") && jointJSONDocument["addRoot"].IsBool())
			gAddRoot = jointJSONDocument["addRoot"].GetBool();
        if (jointJSONDocument.HasMember("addRootChildName") && jointJSONDocument["addRootChildName"].IsString())
            gAddRootChildName = jointJSONDocument["addRootChildName"].GetString();
		if (jointJSONDocument.HasMember("addRootRootName") && jointJSONDocument["addRootRootName"].IsString())
			gAddRootRootName = jointJSONDocument["addRootRootName"].GetString();
        if (jointJSONDocument.HasMember("removeLeafName") && jointJSONDocument["removeLeafName"].IsString())
            gRemoveLeafName = jointJSONDocument["removeLeafName"].GetString();
	}

	return 0;
}


int main(int argc, char** argv)
{
	SetConsoleOutputCP(CP_UTF8);

	bool didEverythingSucceed { true };
	bool isBulk { false };
	std::string inFilePath;
	std::string outFilePath;
	std::string jointMetaFilePath;

	int width = 0;
	std::string name;
	bool doIt = false;
	std::string command;

	auto cli =
		Opt(inFilePath, "input path")
		["-i"] ["--input-files"]
		("path to the input file(s)")
		| Opt(outFilePath, "output path")
		["-o"] ["--output-files"]
		("path for the output file(s)")
		| Opt(isBulk) ["-b"] ["--bulk"]
		("Bulk process more than one file?")
		| Opt(isVerbose)
		["-v"] ["--verbose"]("Output verbose information")
		| Opt(jointMetaFilePath, "Joint meta file")
		["-j"] ["--joints"]
		| Opt(addIK)
		["-k"] ["--add-ik"]("Add standard IK bones to the model")
		| Opt(applyMixamoFixes)
		["-f"] ["--fixamo"]("Apply fixes to Mixamo model")
		| Opt(gScale, "uniform scale")
		["--scale"]("Apply uniform scale");

	auto result = cli.parse(Args(argc, argv));
	if (!result) {
		std::cerr << "Error in command line: " << result.errorMessage() << std::endl;
		cli.writeToStream(std::cout);
		exit(1);
	}

	FbxString fbxInFilePath = StdStr2FbxStr(inFilePath);
	FbxManager* pFbxManager = nullptr;
	FbxScene* pFbxScene = nullptr;

	// Prepare the FBX SDK.
	InitializeSdkObjects(pFbxManager, pFbxScene);
	FBXSDK_printf("\n");

	if (jointMetaFilePath.length() > 0)
	{
		if (int error = ReadJointFile(jointMetaFilePath) != 0)
		{
			std::cerr << "Joint file was mal-formed JSON." << std::endl;
			exit(error);
		}
	}

	if (!isBulk)
	{
		// Default output is to the same file as the input.
		FbxString fbxOutFilePath;
		if (outFilePath.length() > 0)
			fbxOutFilePath = StdStr2FbxStr(outFilePath);
		else
			fbxOutFilePath = fbxInFilePath;

		// There's only one file to process.
		didEverythingSucceed = didEverythingSucceed && ProcessFile(pFbxManager, pFbxScene, fbxInFilePath, fbxOutFilePath);
	}
	else
	{
		bool bAbort = false;
		if (outFilePath.length() == 0)
		{
			std::cerr << "Error: You must supply an output path for bulk conversions." << std::endl;
			cli.writeToStream(std::cout);
			exit(1);
		}

		// Default to using the present directory if no file path was supplied.
		TCHAR* tmpInputPath = nullptr;
		size_t pReturnValue;
		if (fbxInFilePath.IsEmpty())
			FbxAnsiToWC(".", tmpInputPath, &pReturnValue);
		else
			FbxAnsiToWC(inFilePath.c_str(), tmpInputPath, &pReturnValue);

		std::wstring inputRootPath = tmpInputPath;
        TCHAR* tmpOutputPath = nullptr;
        FbxAnsiToWC(outFilePath.c_str(), tmpOutputPath);
        std::wstring outputRootPath = tmpOutputPath;
        delete[] tmpOutputPath;
		delete[] tmpInputPath;

		ProcessDirectory(pFbxManager, pFbxScene, inputRootPath, outputRootPath, inputRootPath);		
	}

	// Destroy all objects created by the FBX SDK.
	FBXSDK_printf("\n");
	DestroySdkObjects(pFbxManager, didEverythingSucceed);

	return 0;
}
