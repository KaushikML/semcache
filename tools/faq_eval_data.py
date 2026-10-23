"""Synthetic FAQ evaluation set (Phase 7), written for this project.

Each intent has a canonical question and paraphrases (from light rewording to different words).
Hard negatives share most of their words with an intent but ask something else; each one is its
own intent, so a cache HIT on it is a false hit. Caveat: written by one author, so wording
variety is narrower than real traffic; QQP complements it.
"""

INTENTS = {
    "refund_policy": ["What is your refund policy?", "what's your refund policy", "Can I get a refund?",
                      "How do refunds work?", "How do I get my money back?"],
    "reset_password": ["How do I reset my password?", "How can I reset my password?", "I forgot my password",
                       "Reset password steps", "I can't log in, how do I change my forgotten password?"],
    "change_email": ["How do I change my email address?", "How can I update my email?",
                     "Change the email on my account", "Where do I edit my e-mail address?",
                     "I want a different email for my login"],
    "cancel_subscription": ["How do I cancel my subscription?", "How can I cancel my subscription?",
                            "Cancel my plan", "I want to stop my membership", "How do I end my monthly plan?"],
    "shipping_time": ["How long does shipping take?", "How long is delivery?", "When will my order arrive?",
                      "What are your delivery times?", "How many days until my package gets here?"],
    "international_shipping": ["Do you ship internationally?", "Do you ship outside the US?",
                               "Can you deliver to other countries?", "Is worldwide shipping available?",
                               "Can I order from Canada?"],
    "payment_methods": ["What payment methods do you accept?", "Which payment options are available?",
                        "Can I pay with PayPal?", "Do you take credit cards?", "How can I pay?"],
    "track_order": ["How do I track my order?", "Where is my order?", "Can I track my package?",
                    "Is there a tracking number for my shipment?", "How do I see where my delivery is?"],
    "return_item": ["How do I return an item?", "How can I send something back?", "What is the return process?",
                    "I want to return my purchase", "Steps to return a product"],
    "contact_support": ["How do I contact customer support?", "How can I reach support?",
                        "What is your support phone number?", "How do I talk to a human?",
                        "Is there a customer service email?"],
    "business_hours": ["What are your business hours?", "When are you open?", "What time does support open?",
                       "Are you open on weekends?", "What are your opening hours?"],
    "student_discount": ["Is there a student discount?", "Do you offer discounts for students?",
                         "Can students get a cheaper price?", "Student pricing?", "Do you have an education discount?"],
    "delete_account": ["How do I delete my account?", "How can I remove my account?", "Delete my profile",
                       "I want to close my account permanently", "How do I erase all my data and account?"],
    "change_plan": ["How do I upgrade my plan?", "How can I switch to a higher plan?", "Upgrade my subscription",
                    "Can I move to the premium tier?", "How do I get more features on my account?"],
    "invoice": ["Where can I find my invoice?", "How do I download my invoice?", "I need a receipt for my payment",
                "Where are my billing documents?", "Can I get a copy of my bill?"],
    "billing_address": ["How do I change my billing address?", "How can I update my billing address?",
                        "Edit the address on my invoices", "My billing address is wrong",
                        "Where do I set the address used for payments?"],
    "two_factor": ["How do I enable two-factor authentication?", "How can I turn on 2FA?",
                   "Set up two step verification", "Does my account support 2FA?", "How do I add an authenticator app?"],
    "data_export": ["How do I export my data?", "Can I download all my data?", "Export my account data",
                    "How do I get a copy of my information?", "Is there a data export feature?"],
    "free_trial": ["Is there a free trial?", "Can I try it for free?", "Do you offer a trial period?",
                   "How long is the free trial?", "Can I test the product before paying?"],
    "api_rate_limit": ["What are the API rate limits?", "How many API requests can I make?", "API request limit",
                       "Is there a cap on API calls per minute?", "How fast can I call your API?"],
    "change_username": ["How do I change my username?", "Can I rename my account?", "Update my user name",
                        "How do I pick a new handle?", "Where can I edit my display name?"],
    "gift_cards": ["Do you sell gift cards?", "Can I buy a gift card?", "Are gift cards available?",
                   "How do I purchase a gift voucher?", "Can I give your product as a present?"],
    "order_cancel": ["How do I cancel my order?", "Can I cancel my order?", "Cancel an order I just placed",
                     "I ordered by mistake, how do I undo it?", "Stop my order before it ships"],
    "warranty": ["What does the warranty cover?", "How long is the warranty?", "Is my product under warranty?",
                 "What is your guarantee policy?", "Will you fix it if it breaks?"],
    "privacy": ["How do you use my personal data?", "What is your privacy policy?", "Do you sell my data?",
                "How is my information protected?", "Who can see my personal information?"],
    "language": ["Can I change the language?", "How do I switch the app language?", "Is the site available in Spanish?",
                 "Change interface language", "How do I set my preferred language?"],
    "dark_mode": ["Is there a dark mode?", "How do I enable dark mode?", "Can I use a dark theme?",
                  "Switch to dark theme", "How do I make the screen darker?"],
    "invite_team": ["How do I invite my team?", "How can I add team members?", "Invite colleagues to my workspace",
                    "Add a user to my organization", "How do I share access with coworkers?"],
    "pricing": ["How much does it cost?", "What are your prices?", "What is the price of the pro plan?",
                "Pricing details", "How expensive is a subscription?"],
    "mobile_app": ["Do you have a mobile app?", "Is there an iPhone app?", "Can I use it on Android?",
                   "Is there an app for my phone?", "Where can I download the mobile application?"],
}

# Close in wording to an intent above, different meaning: a HIT on any of these is a false hit.
HARD_NEGATIVES = [
    "What is your shipping policy?", "How do I reset my router?", "How do I change my phone number?",
    "How do I pause my subscription?", "How long does a refund take?", "Do you ship batteries internationally?",
    "What payment methods do you accept for refunds?", "How do I track my refund?", "How do I return a gift card?",
    "How do I contact the sales team?", "What are your holiday hours?", "Is there a senior discount?",
    "How do I delete a message?", "How do I downgrade my plan?", "Where can I find my order number?",
    "How do I change my shipping address?", "How do I disable two-factor authentication?", "How do I import my data?",
    "Is there a free plan?", "What are the API pricing tiers?", "How do I change my password?",
    "Do you accept gift cards as payment?", "How do I cancel my account?", "What does the warranty not cover?",
    "How do you use cookies?", "Can I change the currency?", "Is there a light mode?", "How do I remove a team member?",
    "How much does shipping cost?", "Do you have a desktop app?",
]


def rows():
    out = []
    for intent, questions in INTENTS.items():
        for i, q in enumerate(questions):
            out.append({"intent": intent, "text": q, "kind": "canonical" if i == 0 else "paraphrase"})
    for i, q in enumerate(HARD_NEGATIVES):
        out.append({"intent": f"negative_{i:02d}", "text": q, "kind": "negative"})
    return out
