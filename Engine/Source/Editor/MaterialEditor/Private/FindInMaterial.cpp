// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.
#include "MaterialEditorModule.h"
#include "Materials/MaterialExpressionComment.h"
#include "FindInMaterial.h"
#include "MaterialEditor.h"
#include "SSearchBox.h"

#define LOCTEXT_NAMESPACE "FindInMaterial"

//////////////////////////////////////////////////////////////////////////
// FFindInMaterialResult

FFindInMaterialResult::FFindInMaterialResult(const FString& InValue)
:Value(InValue), DuplicationIndex(0), Class(NULL), Pin(NULL), GraphNode(NULL)
{
}

FFindInMaterialResult::FFindInMaterialResult(const FString& InValue, TSharedPtr<FFindInMaterialResult>& InParent, UClass* InClass, int InDuplicationIndex)
: Parent(InParent), Value(InValue), DuplicationIndex(InDuplicationIndex), Class(InClass), Pin(NULL), GraphNode(NULL)
{
}

FFindInMaterialResult::FFindInMaterialResult(const FString& InValue, TSharedPtr<FFindInMaterialResult>& InParent, UEdGraphPin* InPin)
: Parent(InParent), Value(InValue), DuplicationIndex(0), Class(InPin->GetClass()), Pin(InPin), GraphNode(NULL)
{
}

FFindInMaterialResult::FFindInMaterialResult(const FString& InValue, TSharedPtr<FFindInMaterialResult>& InParent, UEdGraphNode* InNode)
: Parent(InParent), Value(InValue), DuplicationIndex(0), Class(InNode->GetClass()), Pin(NULL), GraphNode(InNode)
{
	if (GraphNode.IsValid())
	{
		CommentText = GraphNode->NodeComment;
	}
}

FReply FFindInMaterialResult::OnClick(TWeakPtr<class FMaterialEditor> MaterialEditor)
{
	if (GraphNode.IsValid())
	{
		MaterialEditor.Pin()->JumpToNode(GraphNode.Get());
	}
	else if (Pin.IsValid())
	{
		MaterialEditor.Pin()->JumpToNode(Pin.Get()->GetOwningNode());
	}
	return FReply::Handled();
}

FString FFindInMaterialResult::GetCategory() const
{
	if (Class == UEdGraphPin::StaticClass())
	{
		return LOCTEXT("PinCategory", "Pin").ToString();
	}
	else
	{
		return LOCTEXT("NodeCategory", "Node").ToString();
	}
	return FString();
}

TSharedRef<SWidget> FFindInMaterialResult::CreateIcon() const
{
	FSlateColor IconColor = FSlateColor::UseForeground();
	const FSlateBrush* Brush = NULL;
	if (Pin.IsValid())
	{
		if (Pin->PinType.bIsArray)
		{
			Brush = FEditorStyle::GetBrush(TEXT("GraphEditor.ArrayPinIcon"));
		}
		else if (Pin->PinType.bIsReference)
		{
			Brush = FEditorStyle::GetBrush(TEXT("GraphEditor.RefPinIcon"));
		}
		else
		{
			Brush = FEditorStyle::GetBrush(TEXT("GraphEditor.PinIcon"));
		}
		const UEdGraphSchema* Schema = Pin->GetSchema();
		IconColor = Schema->GetPinTypeColor(Pin->PinType);
	}
	else if (GraphNode.IsValid())
	{
		Brush = FEditorStyle::GetBrush(TEXT("GraphEditor.NodeGlyph"));
	}

	return 	SNew(SImage)
		.Image(Brush)
		.ColorAndOpacity(IconColor)
		.ToolTipText(GetCategory());
}

FString FFindInMaterialResult::GetCommentText() const
{
	return CommentText;
}

FString FFindInMaterialResult::GetValueText() const
{
	FString DisplayVal;

	if (Pin.IsValid()
		&& (!Pin->DefaultValue.IsEmpty() || !Pin->AutogeneratedDefaultValue.IsEmpty() || Pin->DefaultObject || !Pin->DefaultTextValue.IsEmpty())
		&& (Pin->Direction == EGPD_Input)
		&& (Pin->LinkedTo.Num() == 0))
	{
		if (Pin->DefaultObject)
		{
			const AActor* DefaultActor = Cast<AActor>(Pin->DefaultObject);
			DisplayVal = FString::Printf(TEXT("(%s)"), DefaultActor ? *DefaultActor->GetActorLabel() : *Pin->DefaultObject->GetName());
		}
		else if (!Pin->DefaultValue.IsEmpty())
		{
			DisplayVal = FString::Printf(TEXT("(%s)"), *Pin->DefaultValue);
		}
		else if (!Pin->AutogeneratedDefaultValue.IsEmpty())
		{
			DisplayVal = FString::Printf(TEXT("(%s)"), *Pin->AutogeneratedDefaultValue);
		}
		else if (!Pin->DefaultTextValue.IsEmpty())
		{
			DisplayVal = FString::Printf(TEXT("(%s)"), *Pin->DefaultTextValue.ToString());
		}
	}

	return DisplayVal;
}

//////////////////////////////////////////////////////////////////////////
// SFindInMaterial

void SFindInMaterial::Construct(const FArguments& InArgs, TSharedPtr<FMaterialEditor> InMaterialEditor)
{
	MaterialEditorPtr = InMaterialEditor;

	this->ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1)
			[
				SAssignNew(SearchTextField, SSearchBox)
				.HintText(LOCTEXT("GraphSearchHint", "Search"))
				.OnTextChanged(this, &SFindInMaterial::OnSearchTextChanged)
				.OnTextCommitted(this, &SFindInMaterial::OnSearchTextCommitted)
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(0.f, 4.f, 0.f, 0.f)
		[
			SNew(SBorder)
			.BorderImage(FEditorStyle::GetBrush("Menu.Background"))
			[
				SAssignNew(TreeView, STreeViewType)
				.ItemHeight(24)
				.TreeItemsSource(&ItemsFound)
				.OnGenerateRow(this, &SFindInMaterial::OnGenerateRow)
				.OnGetChildren(this, &SFindInMaterial::OnGetChildren)
				.OnSelectionChanged(this, &SFindInMaterial::OnTreeSelectionChanged)
				.SelectionMode(ESelectionMode::Multi)
			]
		]
	];
}

void SFindInMaterial::FocusForUse()
{
	// NOTE: Careful, GeneratePathToWidget can be reentrant in that it can call visibility delegates and such
	FWidgetPath FilterTextBoxWidgetPath;
	FSlateApplication::Get().GeneratePathToWidgetUnchecked(SearchTextField.ToSharedRef(), FilterTextBoxWidgetPath);

	// Set keyboard focus directly
	FSlateApplication::Get().SetKeyboardFocus(FilterTextBoxWidgetPath, EFocusCause::SetDirectly);
}

void SFindInMaterial::OnSearchTextChanged(const FText& Text)
{
	SearchValue = Text.ToString();
}

void SFindInMaterial::OnSearchTextCommitted(const FText& Text, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnEnter)
	{
		InitiateSearch();
	}
}

void SFindInMaterial::InitiateSearch()
{
	TArray<FString> Tokens;
	if (SearchValue.Contains("\"") && SearchValue.ParseIntoArray(&Tokens, TEXT("\""), true) > 0)
	{
		for (auto &TokenIt : Tokens)
		{
			// we have the token, we don't need the quotes anymore, they'll just confused the comparison later on
			TokenIt = TokenIt.TrimQuotes();
			// We remove the spaces as all later comparison strings will also be de-spaced
			TokenIt = TokenIt.Replace(TEXT(" "), TEXT(""));
		}

		// due to being able to handle multiple quoted blocks like ("Make Epic" "Game Now") we can end up with
		// and empty string between (" ") blocks so this simply removes them
		struct FRemoveMatchingStrings
		{
			bool operator()(const FString& RemovalCandidate) const
			{
				return RemovalCandidate.IsEmpty();
			}
		};
		Tokens.RemoveAll(FRemoveMatchingStrings());
	}
	else
	{
		// unquoted search equivalent to a match-any-of search
		SearchValue.ParseIntoArray(&Tokens, TEXT(" "), true);
	}

	for (auto It(ItemsFound.CreateIterator()); It; ++It)
	{
		TreeView->SetItemExpansion(*It, false);
	}
	ItemsFound.Empty();
	if (Tokens.Num() > 0)
	{
		HighlightText = FText::FromString(SearchValue);
		MatchTokens(Tokens);
	}

	// Insert a fake result to inform user if none found
	if (ItemsFound.Num() == 0)
	{
		ItemsFound.Add(FSearchResult(new FFindInMaterialResult(LOCTEXT("MaterialSearchNoResults", "No Results found").ToString())));
	}

	TreeView->RequestTreeRefresh();

	for (auto It(ItemsFound.CreateIterator()); It; ++It)
	{
		TreeView->SetItemExpansion(*It, true);
	}
}

void SFindInMaterial::MatchTokens(const TArray<FString> &Tokens)
{
	RootSearchResult.Reset();

	UEdGraph* Graph = MaterialEditorPtr.Pin()->Material->MaterialGraph;

	if (Graph == NULL)
	{
		return;
	}

	RootSearchResult = FSearchResult(new FFindInMaterialResult(FString("BehaviorTreeRoot")));

	for (auto It(Graph->Nodes.CreateConstIterator()); It; ++It)
	{
		UEdGraphNode* Node = *It;

		const FString NodeName = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		FSearchResult NodeResult(new FFindInMaterialResult(NodeName, RootSearchResult, Node));

		FString NodeSearchString = NodeName + Node->NodeComment;
		NodeSearchString = NodeSearchString.Replace(TEXT(" "), TEXT(""));

		bool bNodeMatchesSearch = StringMatchesSearchTokens(Tokens, NodeSearchString);

		// Use old Material Expression search functions too
		if (!bNodeMatchesSearch)
		{
			bool bMatchesAllTokens = true;
			if (UMaterialGraphNode* MatNode = Cast<UMaterialGraphNode>(Node))
			{
				for (int32 Index = 0; Index < Tokens.Num(); ++Index)
				{
					if (!MatNode->MaterialExpression->MatchesSearchQuery(*Tokens[Index]))
					{
						bMatchesAllTokens = false;
						break;
					}
				}
			}
			else if (UMaterialGraphNode_Comment* MatComment = Cast<UMaterialGraphNode_Comment>(Node))
			{
				for (int32 Index = 0; Index < Tokens.Num(); ++Index)
				{
					if (!MatComment->MaterialExpressionComment->MatchesSearchQuery(*Tokens[Index]))
					{
						bMatchesAllTokens = false;
						break;
					}
				}
			}
			else
			{
				bMatchesAllTokens = false;
			}
			if (bMatchesAllTokens)
			{
				bNodeMatchesSearch = true;
			}
		}

		for (TArray<UEdGraphPin*>::TIterator PinIt(Node->Pins); PinIt; ++PinIt)
		{
			UEdGraphPin* Pin = *PinIt;
			if (Pin && Pin->PinFriendlyName.CompareTo(FText::FromString(TEXT(" "))) != 0)
			{
				FString PinName = Pin->GetSchema()->GetPinDisplayName(Pin);
				FString PinSearchString = Pin->PinName + Pin->PinFriendlyName.ToString() + Pin->DefaultValue + Pin->PinType.PinCategory + Pin->PinType.PinSubCategory + (Pin->PinType.PinSubCategoryObject.IsValid() ? Pin->PinType.PinSubCategoryObject.Get()->GetFullName() : TEXT(""));
				PinSearchString = PinSearchString.Replace(TEXT(" "), TEXT(""));
				if (StringMatchesSearchTokens(Tokens, PinSearchString))
				{
					FSearchResult PinResult(new FFindInMaterialResult(PinName, NodeResult, Pin));
					NodeResult->Children.Add(PinResult);
				}
			}
		}

		if ((NodeResult->Children.Num() > 0) || bNodeMatchesSearch)
		{
			ItemsFound.Add(NodeResult);
		}
	}
}

TSharedRef<ITableRow> SFindInMaterial::OnGenerateRow(FSearchResult InItem, const TSharedRef<STableViewBase>& OwnerTable)
{
	bool bIsACategoryWidget = false;//!bIsInFindWithinBlueprintMode && !InItem->Parent.IsValid();

	if (bIsACategoryWidget)
	{
		return SNew(STableRow< TSharedPtr<FFindInMaterialResult> >, OwnerTable)
			[
				SNew(SBorder)
				.VAlign(VAlign_Center)
				.BorderImage(FEditorStyle::GetBrush("PropertyWindow.CategoryBackground"))
				.Padding(FMargin(2.0f))
				.ForegroundColor(FEditorStyle::GetColor("PropertyWindow.CategoryForeground"))
				[
					SNew(STextBlock)
					.Text(InItem->Value)
					.ToolTipText(LOCTEXT("BlueprintCatSearchToolTip", "Blueprint").ToString())
				]
			];
	}
	else // Functions/Event/Pin widget
	{
		FString CommentText = InItem->GetCommentText();

		return SNew(STableRow< TSharedPtr<FFindInMaterialResult> >, OwnerTable)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					InItem->CreateIcon()
				]
				+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(2, 0)
					[
						SNew(STextBlock)
						.Text(InItem->Value)
						.HighlightText(HighlightText)
						.ToolTipText(FString::Printf(*LOCTEXT("BlueprintResultSearchToolTip", "%s : %s").ToString(), *InItem->GetCategory(), *InItem->Value))
					]
				+ SHorizontalBox::Slot()
					.FillWidth(1)
					.HAlign(HAlign_Right)
					.VAlign(VAlign_Center)
					.Padding(2, 0)
					[
						SNew(STextBlock)
						.Text(InItem->GetValueText())
						.HighlightText(HighlightText)
					]
				+ SHorizontalBox::Slot()
					.FillWidth(1)
					.HAlign(HAlign_Right)
					.VAlign(VAlign_Center)
					.Padding(2, 0)
					[
						SNew(STextBlock)
						.Text(CommentText.IsEmpty() ? FString() : FString::Printf(TEXT("Node Comment:[%s]"), *CommentText))
						.ColorAndOpacity(FLinearColor::Yellow)
						.HighlightText(HighlightText)
					]
			];
	}
}

void SFindInMaterial::OnGetChildren(FSearchResult InItem, TArray< FSearchResult >& OutChildren)
{
	OutChildren += InItem->Children;
}

void SFindInMaterial::OnTreeSelectionChanged(FSearchResult Item, ESelectInfo::Type)
{
	if (Item.IsValid())
	{
		Item->OnClick(MaterialEditorPtr);
	}
}

bool SFindInMaterial::StringMatchesSearchTokens(const TArray<FString>& Tokens, const FString& ComparisonString)
{
	bool bFoundAllTokens = true;
	//search the entry for each token, it must have all of them to pass
	for (auto TokIT(Tokens.CreateConstIterator()); TokIT; ++TokIT)
	{
		const FString& Token = *TokIT;
		if (!ComparisonString.Contains(Token))
		{
			bFoundAllTokens = false;
			break;
		}
	}
	return bFoundAllTokens;
}

/////////////////////////////////////////////////////

#undef LOCTEXT_NAMESPACE
