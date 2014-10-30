// Copyright 1998-2014 Epic Games, Inc. All Rights Reserved.

#include "SlateCorePrivatePCH.h"
#include "HittestGrid.h"

DEFINE_LOG_CATEGORY_STATIC(LogHittestDebug, Display, All);

static const FVector2D CellSize(128.0f, 128.0f);

struct FHittestGrid::FCachedWidget
{
	FCachedWidget( int32 InParentIndex, const FArrangedWidget& InWidget, const FSlateRect& InClippingRect )
	: WidgetPtr(InWidget.Widget)
	, CachedGeometry( InWidget.Geometry )
	, ClippingRect( InClippingRect )
	, Children()
	, ParentIndex( InParentIndex )
	{
	}

	void AddChild( const int32 ChildIndex )
	{
		Children.Add( ChildIndex );
	}

	TWeakPtr<SWidget> WidgetPtr;
	/** Allow widgets that implement this interface to insert widgets into the bubble path */
	TWeakPtr<ICustomHitTestPath> CustomPath;
	FGeometry CachedGeometry;
	// @todo umg : ideally this clipping rect is optional and we only have them on a small number of widgets.
	FSlateRect ClippingRect;
	TArray<int32> Children;
	int32 ParentIndex;
};


FHittestGrid::FHittestGrid()
: WidgetsCachedThisFrame( new TArray<FCachedWidget>() )
{
}

TArray<FWidgetAndPointer> FHittestGrid::GetBubblePath( FVector2D DesktopSpaceCoordinate, bool bIgnoreEnabledStatus )
{
	if (WidgetsCachedThisFrame->Num() > 0 && Cells.Num() > 0)
	{
		const FVector2D CursorPositionInGrid = DesktopSpaceCoordinate - GridOrigin;
		const FIntPoint CellCoordinate = FIntPoint(
			FMath::Min( FMath::Max(FMath::FloorToInt(CursorPositionInGrid.X / CellSize.X), 0), NumCells.X-1),
			FMath::Min( FMath::Max(FMath::FloorToInt(CursorPositionInGrid.Y / CellSize.Y), 0), NumCells.Y-1 ) );
		
		static FVector2D LastCoordinate = FVector2D::ZeroVector;
		if ( LastCoordinate != CursorPositionInGrid )
		{
			LastCoordinate = CursorPositionInGrid;
		}

		checkf( (CellCoordinate.Y*NumCells.X + CellCoordinate.X) < Cells.Num(), TEXT("Index out of range, CellCoordinate is: %d %d CursorPosition is: %f %f"), CellCoordinate.X, CellCoordinate.Y, CursorPositionInGrid.X, CursorPositionInGrid.Y );

		const TArray<int32>& IndexesInCell = CellAt( CellCoordinate.X, CellCoordinate.Y ).CachedWidgetIndexes;
		int32 HitWidgetIndex = INDEX_NONE;
	
		// Consider front-most widgets first for hittesting.
		for ( int32 i = IndexesInCell.Num()-1; i>=0 && HitWidgetIndex==INDEX_NONE; --i )
		{
			check( IndexesInCell[i] < WidgetsCachedThisFrame->Num() ); 

			const FCachedWidget& TestCandidate = (*WidgetsCachedThisFrame)[IndexesInCell[i]];

			// Compute the render space clipping rect (FGeometry exposes a layout space clipping rect).
			FSlateRotatedRect DesktopOrientedClipRect = 
				TransformRect(
					Concatenate(
						Inverse(TestCandidate.CachedGeometry.GetAccumulatedLayoutTransform()), 
						TestCandidate.CachedGeometry.GetAccumulatedRenderTransform()
					), 
					FSlateRotatedRect(TestCandidate.CachedGeometry.GetClippingRect().IntersectionWith(TestCandidate.ClippingRect))
				);

			if (DesktopOrientedClipRect.IsUnderLocation(DesktopSpaceCoordinate) && TestCandidate.WidgetPtr.IsValid())
			{
				HitWidgetIndex = IndexesInCell[i];
			}
		}
		
		TArray<FWidgetAndPointer> BubblePath =[&]()
		{
			if( HitWidgetIndex != INDEX_NONE )
			{
				const FCachedWidget& PhysicallyHitWidget = (*WidgetsCachedThisFrame)[HitWidgetIndex];
				if(PhysicallyHitWidget.CustomPath.IsValid())
				{
					return PhysicallyHitWidget.CustomPath.Pin()->GetBubblePathAndVirtualCursors(PhysicallyHitWidget.CachedGeometry, DesktopSpaceCoordinate, bIgnoreEnabledStatus);
				}
			}

			return TArray<FWidgetAndPointer>();

		}();

		if (HitWidgetIndex != INDEX_NONE)
		{
			int32 CurWidgetIndex=HitWidgetIndex;
			bool bPathUninterrupted = false;
			do
			{
				check( CurWidgetIndex < WidgetsCachedThisFrame->Num() );
				const FCachedWidget& CurCachedWidget = (*WidgetsCachedThisFrame)[CurWidgetIndex];
				const TSharedPtr<SWidget> CachedWidgetPtr = CurCachedWidget.WidgetPtr.Pin();

		
				bPathUninterrupted = CachedWidgetPtr.IsValid();
				if (bPathUninterrupted)
				{
					BubblePath.Insert( FWidgetAndPointer( FArrangedWidget(CachedWidgetPtr.ToSharedRef(), CurCachedWidget.CachedGeometry), TSharedPtr<FVirtualPointerPosition>() ), 0 );
					CurWidgetIndex = CurCachedWidget.ParentIndex;
				}
			}
			while (CurWidgetIndex != INDEX_NONE && bPathUninterrupted);
			
			if (!bPathUninterrupted)
			{
				// A widget in the path to the root has been removed, so anything
				// we thought we had hittest is no longer actually there.
				// Pretend we didn't hit anything.
				BubblePath = TArray<FWidgetAndPointer>();
			}

			// Disabling a widget disables all of its logical children
			// This effect is achieved by truncating the path to the
			// root-most enabled widget.
			if ( !bIgnoreEnabledStatus )
			{
				const int32 DisabledWidgetIndex = BubblePath.IndexOfByPredicate( []( const FArrangedWidget& SomeWidget ){ return !SomeWidget.Widget->IsEnabled( ); } );
				if (DisabledWidgetIndex != INDEX_NONE)
				{
					BubblePath.RemoveAt( DisabledWidgetIndex, BubblePath.Num() - DisabledWidgetIndex );
				}
			}
			
			return BubblePath;
		}
		else
		{
			return TArray<FWidgetAndPointer>();
		}
	}
	else
	{
		return TArray<FWidgetAndPointer>();
	}
}

void FHittestGrid::BeginFrame( const FSlateRect& HittestArea )
{
	//LogGrid();

	GridOrigin = HittestArea.GetTopLeft();
	const FVector2D GridSize = HittestArea.GetSize();
	NumCells = FIntPoint( FMath::CeilToInt(GridSize.X / CellSize.X), FMath::CeilToInt(GridSize.Y / CellSize.Y) );
	WidgetsCachedThisFrame->Empty();
	Cells.Reset( NumCells.X * NumCells.Y );	
	Cells.SetNum( NumCells.X * NumCells.Y );
}

int32 FHittestGrid::InsertWidget( const int32 ParentHittestIndex, const EVisibility& Visibility, const FArrangedWidget& Widget, const FVector2D InWindowOffset, const FSlateRect& InClippingRect )
{
	check( ParentHittestIndex < WidgetsCachedThisFrame->Num() );

	// Update the FGeometry to transform into desktop space.
	FArrangedWidget WindowAdjustedWidget(Widget);
	WindowAdjustedWidget.Geometry.AppendTransform(FSlateLayoutTransform(InWindowOffset));
	const FSlateRect WindowAdjustedRect = InClippingRect.OffsetBy(InWindowOffset);

	// Remember this widget, its geometry, and its place in the logical hierarchy.
	const int32 WidgetIndex = WidgetsCachedThisFrame->Add( FCachedWidget( ParentHittestIndex, WindowAdjustedWidget, WindowAdjustedRect ) );
	check( WidgetIndex < WidgetsCachedThisFrame->Num() ); 
	if (ParentHittestIndex != INDEX_NONE)
	{
		(*WidgetsCachedThisFrame)[ParentHittestIndex].AddChild( WidgetIndex );
	}
	
	if (Visibility.IsHitTestVisible())
	{
		// Mark any cell that is overlapped by this widget.

		// Compute the render space clipping rect, and compute it's aligned bounds so we can insert conservatively into the hit test grid.
		FSlateRect GridRelativeBoundingClipRect = 
			TransformRect(
				Concatenate(
					Inverse(WindowAdjustedWidget.Geometry.GetAccumulatedLayoutTransform()),
					WindowAdjustedWidget.Geometry.GetAccumulatedRenderTransform()
				),
				FSlateRotatedRect(WindowAdjustedWidget.Geometry.GetClippingRect().IntersectionWith(WindowAdjustedRect))
			)
			.ToBoundingRect()
			.OffsetBy(-GridOrigin);

		// Starting and ending cells covered by this widget.	
		const FIntPoint UpperLeftCell = FIntPoint(
			FMath::Max(0, FMath::FloorToInt(GridRelativeBoundingClipRect.Left / CellSize.X)),
			FMath::Max(0, FMath::FloorToInt(GridRelativeBoundingClipRect.Top / CellSize.Y)));

		const FIntPoint LowerRightCell = FIntPoint(
			FMath::Min( NumCells.X-1, FMath::FloorToInt(GridRelativeBoundingClipRect.Right / CellSize.X)),
			FMath::Min( NumCells.Y-1, FMath::FloorToInt(GridRelativeBoundingClipRect.Bottom / CellSize.Y)));

		for (int32 XIndex=UpperLeftCell.X; XIndex <= LowerRightCell.X; ++ XIndex )
		{
			for(int32 YIndex=UpperLeftCell.Y; YIndex <= LowerRightCell.Y; ++YIndex)
			{
				CellAt(XIndex, YIndex).CachedWidgetIndexes.Add( WidgetIndex );
			}
		}
	}
	
	return WidgetIndex;

}

void FHittestGrid::InsertCustomHitTestPath( TSharedRef<ICustomHitTestPath> CustomHitTestPath, int32 WidgetIndex )
{
	if( WidgetsCachedThisFrame->IsValidIndex( WidgetIndex ) )
	{
		(*WidgetsCachedThisFrame)[WidgetIndex].CustomPath = CustomHitTestPath;
	}
}

bool FHittestGrid::IsDecendantOf(const TSharedRef<SWidget> Parent, const FCachedWidget& Child)
{
	TSharedPtr<SWidget> ChildWidget = Child.WidgetPtr.Pin();

	if (!ChildWidget.IsValid() || ChildWidget == Parent)
	{
		return false;
	}

	int32 CurWidgetIndex = Child.ParentIndex;
	while (CurWidgetIndex != INDEX_NONE)
	{
		const FCachedWidget& CurCachedWidget = (*WidgetsCachedThisFrame)[CurWidgetIndex];
		CurWidgetIndex = CurCachedWidget.ParentIndex;

		if (Parent == CurCachedWidget.WidgetPtr.Pin())
		{
			return true;
		}
	}
		
	return false;
}

template<typename TCompareFunc, typename TSourceSideFunc, typename TDestSideFunc>
TSharedPtr<SWidget> FHittestGrid::FindFocusableWidget(FSlateRect WidgetRect, const FSlateRect SweptRect, int32 AxisIndex, int32 Increment, const EUINavigation Direction, const FNavigationReply& NavigationReply, TCompareFunc CompareFunc, TSourceSideFunc SourceSideFunc, TDestSideFunc DestSideFunc)
{
	FIntPoint CurrentCellPoint = GetCellCoordinate(WidgetRect.GetCenter());

	int32 StartingIndex = CurrentCellPoint[AxisIndex];

	float CurrentSourceSide = SourceSideFunc(WidgetRect);

	int32 StrideAxis, StrideAxisMin, StrideAxisMax;
	if (AxisIndex == 0)
	{
		StrideAxis = 1;
		StrideAxisMin = FMath::Min(FMath::Max(FMath::FloorToInt(SweptRect.Top / CellSize.Y), 0), NumCells.Y - 1);
		StrideAxisMax = FMath::Min(FMath::Max(FMath::FloorToInt(SweptRect.Bottom / CellSize.Y), 0), NumCells.Y - 1);
	}
	else
	{
		StrideAxis = 0;
		StrideAxisMin = FMath::Min(FMath::Max(FMath::FloorToInt(SweptRect.Left / CellSize.X), 0), NumCells.X - 1);
		StrideAxisMax = FMath::Min(FMath::Max(FMath::FloorToInt(SweptRect.Right / CellSize.X), 0), NumCells.X - 1);
	}

	bool bWrapped = false;
	while (CurrentCellPoint[AxisIndex] >= 0 && CurrentCellPoint[AxisIndex] < NumCells[AxisIndex])
	{
		FIntPoint StrideCellPoint = CurrentCellPoint;
		int32 CurrentCellProcessed = CurrentCellPoint[AxisIndex];
		
		// Increment before the search as a wrap case will change our current cell.
		CurrentCellPoint[AxisIndex] += Increment;
		
		FSlateRect BestWidgetRect;
		TSharedPtr<SWidget> BestWidget = TSharedPtr<SWidget>();
		
		for (StrideCellPoint[StrideAxis] = StrideAxisMin; StrideCellPoint[StrideAxis] <= StrideAxisMax; ++StrideCellPoint[StrideAxis])
		{
			FHittestGrid::FCell& Cell = FHittestGrid::CellAt(StrideCellPoint.X, StrideCellPoint.Y);
			const TArray<int32>& IndexesInCell = Cell.CachedWidgetIndexes;

			for (int32 i = IndexesInCell.Num() - 1; i >= 0; --i)
			{
				int32 CurrentIndex = IndexesInCell[i];
				check(CurrentIndex < WidgetsCachedThisFrame->Num());

				const FCachedWidget& TestCandidate = (*WidgetsCachedThisFrame)[CurrentIndex];
				FSlateRect TestCandidateRect = TestCandidate.ClippingRect;

				if (CompareFunc(DestSideFunc(TestCandidateRect), CurrentSourceSide) && FSlateRect::DoRectanglesIntersect(SweptRect, TestCandidateRect))
				{
					// If this found widget isn't closer then the previously found widget then keep looking.
					if (BestWidget.IsValid() && !CompareFunc(DestSideFunc(BestWidgetRect), DestSideFunc(TestCandidateRect)))
					{
						continue;
					}

					// If we have a non escape boundary condition and this widget isn't a descendant of our boundary condition widget then it's invalid so we keep looking.
					if (NavigationReply.GetBoundaryRule() != EUINavigationRule::Escape 
						&& NavigationReply.GetHandler().IsValid() 
						&& !IsDecendantOf(NavigationReply.GetHandler().ToSharedRef(), TestCandidate))
					{
						continue;
					}

					TSharedPtr<SWidget> Widget = TestCandidate.WidgetPtr.Pin();
					if (Widget.IsValid() && Widget->IsEnabled() && Widget->SupportsKeyboardFocus())
					{
						BestWidgetRect = TestCandidateRect;
						BestWidget = Widget;
					}
				}
			}
		}

		if (BestWidget.IsValid())
		{
			// Check for the need to apply our rule
			if (CompareFunc(DestSideFunc(BestWidgetRect), SourceSideFunc(SweptRect)))
			{
				switch (NavigationReply.GetBoundaryRule())
				{
				case EUINavigationRule::Explicit:
					return NavigationReply.GetFocusRecipient();
				case EUINavigationRule::Custom:
					{
						const FNavigationDelegate& FocusDelegate = NavigationReply.GetFocusDelegate();
						if (FocusDelegate.IsBound())
						{
							return FocusDelegate.Execute(Direction);
						}
						return TSharedPtr<SWidget>();
					}
				case EUINavigationRule::Stop:
					return TSharedPtr<SWidget>();
				case EUINavigationRule::Wrap:
					CurrentSourceSide = DestSideFunc(SweptRect);
					FVector2D SampleSpot = WidgetRect.GetCenter();
					SampleSpot[AxisIndex] = CurrentSourceSide;
					CurrentCellPoint = GetCellCoordinate(SampleSpot);
					bWrapped = true;
					break;
				}
			}

			return BestWidget;
		}

		// break if we have looped back to where we started.
		if (bWrapped && StartingIndex == CurrentCellProcessed) { break; }

		// If were going to fail our bounds check and our rule is to wrap then wrap our position
		if (!(CurrentCellPoint[AxisIndex] >= 0 && CurrentCellPoint[AxisIndex] < NumCells[AxisIndex]) && NavigationReply.GetBoundaryRule() == EUINavigationRule::Wrap)
		{
			CurrentSourceSide = DestSideFunc(SweptRect);
			FVector2D SampleSpot = WidgetRect.GetCenter();
			SampleSpot[AxisIndex] = CurrentSourceSide;
			CurrentCellPoint = GetCellCoordinate(SampleSpot);
			bWrapped = true;
		}
	}

	return TSharedPtr<SWidget>();
}

TSharedPtr<SWidget> FHittestGrid::FindNextFocusableWidget(const FArrangedWidget& StartingWidget, const EUINavigation Direction, const FNavigationReply& NavigationReply, const FArrangedWidget& RuleWidget)
{
	FSlateRect WidgetRect = 
		TransformRect(
			Concatenate(
				Inverse(StartingWidget.Geometry.GetAccumulatedLayoutTransform()),
				StartingWidget.Geometry.GetAccumulatedRenderTransform()
			),
			FSlateRotatedRect(StartingWidget.Geometry.GetClippingRect())
		)
		.ToBoundingRect()
		.OffsetBy(-GridOrigin);

	FSlateRect BoundingRuleRect = 
		TransformRect(
			Concatenate(
				Inverse(RuleWidget.Geometry.GetAccumulatedLayoutTransform()),
				RuleWidget.Geometry.GetAccumulatedRenderTransform()
			),
			FSlateRotatedRect(RuleWidget.Geometry.GetClippingRect())
		)
		.ToBoundingRect()
		.OffsetBy(-GridOrigin);

	FSlateRect SweptWidgetRect = WidgetRect;

	TSharedPtr<SWidget> Widget = TSharedPtr<SWidget>();

	switch (Direction)
	{
	case EUINavigation::Left:
		SweptWidgetRect.Left = BoundingRuleRect.Left;
		SweptWidgetRect.Right = BoundingRuleRect.Right;
		SweptWidgetRect.Top += 0.5f;
		SweptWidgetRect.Bottom -= 0.5f;
		Widget = FindFocusableWidget(WidgetRect, SweptWidgetRect, 0, -1, Direction, NavigationReply,
			[](float A, float B) { return A - 0.1f < B; }, // Compare function
			[](FSlateRect SourceRect) { return SourceRect.Left; }, // Source side function
			[](FSlateRect DestRect) { return DestRect.Right; }); // Dest side function
		break;
	case EUINavigation::Right:
		SweptWidgetRect.Left = BoundingRuleRect.Left;
		SweptWidgetRect.Right = BoundingRuleRect.Right;
		SweptWidgetRect.Top += 0.5f;
		SweptWidgetRect.Bottom -= 0.5f;
		Widget = FindFocusableWidget(WidgetRect, SweptWidgetRect, 0, 1, Direction, NavigationReply,
			[](float A, float B) { return A + 0.1f > B; }, // Compare function
			[](FSlateRect SourceRect) { return SourceRect.Right; }, // Source side function
			[](FSlateRect DestRect) { return DestRect.Left; }); // Dest side function
		break;
	case EUINavigation::Up:
		SweptWidgetRect.Top = BoundingRuleRect.Top;
		SweptWidgetRect.Bottom = BoundingRuleRect.Bottom;
		SweptWidgetRect.Left += 0.5f;
		SweptWidgetRect.Right -= 0.5f;
		Widget = FindFocusableWidget(WidgetRect, SweptWidgetRect, 1, -1, Direction, NavigationReply,
			[](float A, float B) { return A - 0.1f < B; }, // Compare function
			[](FSlateRect SourceRect) { return SourceRect.Top; }, // Source side function
			[](FSlateRect DestRect) { return DestRect.Bottom; }); // Dest side function
		break;
	case EUINavigation::Down:
		SweptWidgetRect.Top = BoundingRuleRect.Top;
		SweptWidgetRect.Bottom = BoundingRuleRect.Bottom;
		SweptWidgetRect.Left += 0.5f;
		SweptWidgetRect.Right -= 0.5f;
		Widget = FindFocusableWidget(WidgetRect, SweptWidgetRect, 1, 1, Direction, NavigationReply,
			[](float A, float B) { return A + 0.1f > B; }, // Compare function
			[](FSlateRect SourceRect) { return SourceRect.Bottom; }, // Source side function
			[](FSlateRect DestRect) { return DestRect.Top; }); // Dest side function
		break;

	default:
		break;
	}

	return Widget;
}



FIntPoint FHittestGrid::GetCellCoordinate(FVector2D Position)
{
	return FIntPoint(
		FMath::Min(FMath::Max(FMath::FloorToInt(Position.X / CellSize.X), 0), NumCells.X - 1),
		FMath::Min(FMath::Max(FMath::FloorToInt(Position.Y / CellSize.Y), 0), NumCells.Y - 1));
}

FHittestGrid::FCell& FHittestGrid::CellAt( const int32 X, const int32 Y )
{
	check( (Y*NumCells.X + X) < Cells.Num() );
	return Cells[ Y*NumCells.X + X ];
}

const FHittestGrid::FCell& FHittestGrid::CellAt( const int32 X, const int32 Y ) const
{
	check( (Y*NumCells.X + X) < Cells.Num() );
	return Cells[ Y*NumCells.X + X ];
}


void FHittestGrid::LogGrid() const
{
	FString TempString;
	for (int y=0; y<NumCells.Y; ++y)
	{
		for (int x=0; x<NumCells.X; ++x)
		{
			TempString += "\t";
			TempString += "[";
			for( int i : CellAt(x, y).CachedWidgetIndexes )
			{
				TempString += FString::Printf(TEXT("%d,"), i);
			}
			TempString += "]";
		}
		TempString += "\n";
	}

	TempString += "\n";

	UE_LOG( LogHittestDebug, Warning,TEXT("\n%s"), *TempString );

	for ( int i=0; i<WidgetsCachedThisFrame->Num(); ++i )
	{
		if ( (*WidgetsCachedThisFrame)[i].ParentIndex == INDEX_NONE )
		{
			LogChildren( i, 0, *WidgetsCachedThisFrame );
		}
	}
	

}


void FHittestGrid::LogChildren(int32 Index, int32 IndentLevel, const TArray<FCachedWidget>& WidgetsCachedThisFrame)
{
	FString IndentString;
	for (int i = 0; i<IndentLevel; ++i)
	{
		IndentString += "|\t";
	}

	const FCachedWidget& CachedWidget = WidgetsCachedThisFrame[Index];
	const TSharedPtr<SWidget> CachedWidgetPtr = CachedWidget.WidgetPtr.Pin();
	const FString WidgetString = CachedWidgetPtr.IsValid() ? CachedWidgetPtr->ToString() : TEXT("(null)");
	UE_LOG( LogHittestDebug, Warning, TEXT("%s[%d] => %s @ %s"), *IndentString, Index, *WidgetString , *CachedWidget.CachedGeometry.ToString() );

	for ( int i=0; i<CachedWidget.Children.Num(); ++i )
	{
		LogChildren( CachedWidget.Children[i], IndentLevel+1, WidgetsCachedThisFrame );
	}
}
